/* File: countpairs_wp.c */
/*
  This file is a part of the Corrfunc package
  Copyright (C) 2015-- Manodeep Sinha (manodeep@gmail.com)
  License: MIT LICENSE. See LICENSE file under the top-level
  directory at https://github.com/manodeep/Corrfunc/
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <stdint.h>
#include <inttypes.h>

#include "defs.h"
#include "function_precision.h"
#include "cellarray.h" //definition of struct cellarray_index
#include "utils.h" //all of the utilities
#include "gridlink.h"//function proto-type for gridlink
#include "countpairs_wp.h" //function proto-type

#include "wp_kernels.h" //all kernel proto-types

#ifndef SILENT
#include "progressbar.h" //for the progressbar
#endif

#include "sglib.h"

#if defined(USE_OMP) && defined(_OPENMP)
#include <omp.h>
#endif


#ifndef PERIODIC
#warning "wp is only valid for PERIODIC boundary conditions. Ignoring the Makefile (non)-definition of PERIODIC"
#endif

void free_results_wp(results_countpairs_wp **results)
{
    if(results == NULL)
        return;

    if(*results == NULL)
        return;

    results_countpairs_wp *tmp = *results;
    free(tmp->npairs);
    free(tmp->rupp);
    free(tmp->wp);
    free(tmp->rpavg);
    free(tmp);
    tmp = NULL;
}



results_countpairs_wp *countpairs_wp(const int64_t ND, DOUBLE * restrict X, DOUBLE * restrict Y, DOUBLE * restrict Z,
                                     const double boxsize,
#if defined(USE_OMP) && defined(_OPENMP)
                                     const int numthreads,
#endif
                                     const char *binfile,
                                     const double pimax)
{

    int bin_refine_factor=2,zbin_refine_factor=1;
    int nmesh_x, nmesh_y, nmesh_z;


    /* #if defined(USE_OMP) && defined(_OPENMP) */
    /*  if(numthreads == 1) { */
    /*      bin_refine_factor=2; */
    /*      zbin_refine_factor=1; */
    /*  } else { */
    /*      //seems redundant - but I can not discount the possibility that some */
    /*      //combination of these refine factors will be faster on a different architecture. */
    /*      bin_refine_factor=2; */
    /*      zbin_refine_factor=1; */
    /*  } */
    /* #endif */

    /***********************
     *initializing the  bins
     ************************/
    double *rupp;
    double rpmin,rpmax;
    int nrpbins;
    setup_bins(binfile,&rpmin,&rpmax,&nrpbins,&rupp);
    assert(rpmin > 0.0 && rpmax > 0.0 && rpmin < rpmax && "[rpmin, rpmax] are valid inputs");
    assert(nrpbins > 0 && "Number of rp bins is valid");

    const DOUBLE xmin = 0.0, xmax=boxsize;
    const DOUBLE ymin = 0.0, ymax=boxsize;
    const DOUBLE zmin = 0.0, zmax=boxsize;
    DOUBLE rupp_sqr[nrpbins];
    for(int i=0;i<nrpbins;i++) {
        rupp_sqr[i] = rupp[i]*rupp[i];
    }

    if(rpmax < 0.05*boxsize) bin_refine_factor = 1;
    
    const DOUBLE sqr_rpmin = rupp_sqr[0];
    const DOUBLE sqr_rpmax = rupp_sqr[nrpbins-1];

    //set up the 3-d grid structure. Each element of the structure contains a
    //pointer to the cellarray structure that itself contains all the points
    cellarray_index *lattice = gridlink_index(ND, X, Y, Z, xmin, xmax, ymin, ymax, zmin, zmax, rpmax, rpmax, pimax, bin_refine_factor, bin_refine_factor, zbin_refine_factor, &nmesh_x, &nmesh_y, &nmesh_z);
    if(nmesh_x <= 10 && nmesh_y <= 10 && nmesh_z <= 10) {
        fprintf(stderr,"countpairs_wp> gridlink seems inefficient - boosting bin refine factor - should lead to better performance\n");
        bin_refine_factor *=2;
        zbin_refine_factor *=2;
        const int64_t totncells = nmesh_x*nmesh_y*(int64_t) nmesh_z;
        free_cellarray_index(lattice, totncells);
        lattice = gridlink_index(ND, X, Y, Z, xmin, xmax, ymin, ymax, zmin, zmax, rpmax, rpmax, pimax, bin_refine_factor, bin_refine_factor, zbin_refine_factor, &nmesh_x, &nmesh_y, &nmesh_z);
    }
    const int64_t totncells = nmesh_x*nmesh_y*(int64_t) nmesh_z;


#if defined(USE_OMP) && defined(_OPENMP)
    omp_set_num_threads(numthreads);
#pragma omp parallel for schedule(dynamic)
#endif
    for(int64_t icell=0;icell<totncells;icell++) {
        const cellarray_index *first=&(lattice[icell]);
        if(first->nelements > 0) {
          const int64_t start = first->start;
          DOUBLE *x = X + start;
          DOUBLE *y = Y + start;
          DOUBLE *z = Z + start;
          
#define MULTIPLE_ARRAY_EXCHANGER(type,a,i,j) { SGLIB_ARRAY_ELEMENTS_EXCHANGER(DOUBLE,x,i,j); \
            SGLIB_ARRAY_ELEMENTS_EXCHANGER(DOUBLE,y,i,j);               \
            SGLIB_ARRAY_ELEMENTS_EXCHANGER(DOUBLE,z,i,j) }
          
          SGLIB_ARRAY_QUICK_SORT(DOUBLE, z, first->nelements, SGLIB_NUMERIC_COMPARATOR , MULTIPLE_ARRAY_EXCHANGER);
#undef MULTIPLE_ARRAY_EXCHANGER
        }
    }

#if defined(USE_OMP) && defined(_OPENMP)
    omp_set_num_threads(numthreads);
    uint64_t **all_npairs = (uint64_t **) matrix_calloc(sizeof(uint64_t), numthreads, nrpbins);
#ifdef OUTPUT_RPAVG
    DOUBLE **all_rpavg = (DOUBLE **) matrix_calloc(sizeof(DOUBLE), numthreads, nrpbins);
#endif//OUTPUT_RPAVG

#else//USE_OMP
    uint64_t npair[nrpbins];
    for(int i=0;i<nrpbins;i++) {
        npair[i]=0;
    }
#ifdef OUTPUT_RPAVG
    DOUBLE rpavg[nrpbins];
    for(int i=0;i<nrpbins;i++) {
        rpavg[i]=0;
    }
#endif//OUTPUT_RPAVG
#endif// USE_OMP

#ifndef SILENT
    int interrupted=0;
    int64_t numdone=0;
    init_my_progressbar(totncells,&interrupted);
#endif    

    
#if defined(USE_OMP) && defined(_OPENMP)
#ifndef SILENT
#pragma omp parallel shared(numdone)
#else
#pragma omp parallel    
#endif//SILENT    
    {
        const int tid = omp_get_thread_num();
        uint64_t npair[nrpbins];
        for(int i=0;i<nrpbins;i++) {
            npair[i]=0;
        }
#ifdef OUTPUT_RPAVG
        DOUBLE rpavg[nrpbins];
        for(int i=0;i<nrpbins;i++) {
            rpavg[i]=0.0;
        }
#endif


#pragma omp for schedule(dynamic) nowait
#endif//USE_OMP
        for(int index1=0;index1<totncells;index1++) {


#ifndef SILENT          
#if defined(USE_OMP) && defined(_OPENMP)
            if (omp_get_thread_num() == 0)
#endif
                my_progressbar(numdone,&interrupted);


#if defined(USE_OMP) && defined(_OPENMP)
#pragma omp atomic
#endif
            numdone++;
#endif//SILENT

            
            /* First do the same-cell calculations */
            const cellarray_index *first  = &(lattice[index1]);
            if(first->nelements == 0) {
                continue;
            }
            
            same_cell_wp_driver(first,
                                X, Y, Z,
                                sqr_rpmax, sqr_rpmin, nrpbins, rupp_sqr, pimax
#ifdef OUTPUT_RPAVG
                                ,rpavg
#endif
                                ,npair);

            for(int64_t ngb=0;ngb<first->num_ngb;ngb++){
                cellarray_index *second = first->ngb_cells[ngb];
                const DOUBLE off_xwrap = first->xwrap[ngb];
                const DOUBLE off_ywrap = first->ywrap[ngb];
                const DOUBLE off_zwrap = first->zwrap[ngb];
                diff_cells_wp_driver(first, second,
                                     X, Y, Z,
                                     sqr_rpmax, sqr_rpmin, nrpbins, rupp_sqr, pimax,
                                     off_xwrap, off_ywrap, off_zwrap
#ifdef OUTPUT_RPAVG
                                         ,rpavg
#endif
                                         ,npair);
                
            }//ngb loop
        }//index1 loop
#if defined(USE_OMP) && defined(_OPENMP)
        for(int j=0;j<nrpbins;j++) {
            all_npairs[tid][j] = npair[j];
#ifdef OUTPUT_RPAVG
            all_rpavg[tid][j] = rpavg[j];
#endif
        }
    }//omp parallel
#endif

#ifndef SILENT    
    finish_myprogressbar(&interrupted);
#endif


#if defined(USE_OMP) && defined(_OPENMP)
    uint64_t npair[nrpbins];
#ifdef OUTPUT_RPAVG
    DOUBLE rpavg[nrpbins];
#endif
    for(int i=0;i<nrpbins;i++) {
        npair[i] = 0;
#ifdef OUTPUT_RPAVG
        rpavg[i] = 0.0;
#endif
    }

    for(int i=0;i<numthreads;i++) {
        for(int j=0;j<nrpbins;j++) {
            npair[j] += all_npairs[i][j];
#ifdef OUTPUT_RPAVG
            rpavg[j] += all_rpavg[i][j];
#endif
        }
    }
    matrix_free((void **) all_npairs,numthreads);
#ifdef OUTPUT_RPAVG
    matrix_free((void **) all_rpavg, numthreads);
#endif //OUTPUT_RPAVG
#endif//USE_OMP

#ifdef OUTPUT_RPAVG
    for(int i=0;i<nrpbins;i++) {
        if(npair[i] > 0) {
            rpavg[i] /= (DOUBLE) npair[i];
        }
    }
#endif

    free_cellarray_index(lattice, totncells);

    //Pack in the results
    results_countpairs_wp *results = my_malloc(sizeof(*results), 1);
    results->nbin  = nrpbins;
    results->pimax = pimax;
    results->npairs = my_malloc(sizeof(uint64_t), nrpbins);
    results->wp = my_malloc(sizeof(DOUBLE), nrpbins);
    results->rupp   = my_malloc(sizeof(DOUBLE), nrpbins);
    results->rpavg  = my_malloc(sizeof(DOUBLE), nrpbins);


    const DOUBLE avgweight2 = 1.0, avgweight1 = 1.0;
    const DOUBLE density=0.5*avgweight2*ND/(boxsize*boxsize*boxsize);//pairs are not double-counted
    DOUBLE rlow=0.0 ;
    DOUBLE prefac_density_DD=avgweight1*ND*density;
    DOUBLE twice_pimax = 2.0*pimax;

    for(int i=0;i<nrpbins;i++) {
        results->npairs[i] = npair[i];
        results->rupp[i] = rupp[i];
#ifdef OUTPUT_RPAVG
        results->rpavg[i] = rpavg[i];
#else
        results->rpavg[i] = 0.0;
#endif
        const DOUBLE weight0 = (DOUBLE) results->npairs[i];
        /* compute xi, dividing summed weight by that expected for a random set */
        const DOUBLE vol=M_PI*(results->rupp[i]*results->rupp[i]-rlow*rlow)*twice_pimax;
        const DOUBLE weightrandom = prefac_density_DD*vol;
        results->wp[i] = (weight0/weightrandom-1)*twice_pimax;
        rlow=results->rupp[i];
    }
    free(rupp);
    return results;
}
