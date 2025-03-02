/* File: io.c */
/*
  This file is a part of the Corrfunc package
  Copyright (C) 2015-- Manodeep Sinha (manodeep@gmail.com)
  License: MIT LICENSE. See LICENSE file under the top-level
  directory at https://github.com/manodeep/Corrfunc/
*/

#include "io.h"


#ifndef MEMORY_INCREASE_FAC
#define MEMORY_INCREASE_FAC 1.1
#endif

#ifndef MAXLEN
#define MAXLEN 500
#endif


int64_t read_positions(const char *filename, const char *format, const size_t size, const int num_fields, ...)
{
    int64_t np;
    assert(num_fields >= 1 && "You have to request at least one field to read-in");
    assert((size == 4 || size == 8) && "Size of each position element can be either 4 (float) or 8 (double)");

    void *data[num_fields];
    {
        //new scope - just to check if file is gzipped.
        //in that case, use gunzip to unzip the file.
        // *ALL* file open calls in this scope
        // are to fopen and *NOT* my_fopen.

        FILE *fp = fopen(filename,"r");
        if(fp == NULL) {
            /* file does not exist. let's see if the filename.gz file does */
            char buf[MAXLEN] = "";
            my_snprintf(buf, MAXLEN,"%s.gz",filename);
            fp = fopen(buf,"r");
            if (fp == NULL) {
                /* well then, file not found*/
                fprintf(stderr,"ERROR: Could not find file: neither as `%s' nor as `%s'\n",filename,buf);
                exit(EXIT_FAILURE);
            } else {
                /* found the gzipped file. Use a system call to uncompress. */
                fclose(fp);

                /*
                  Note, I am using `filename` rather than `buf` both
                  because C standards say that using the same buffer
                  as both source and destination is *undefined behaviour*.

                  Check under "NOTES", towards the end of "man 3 snprintf".
                */
                my_snprintf(buf,MAXLEN,"gunzip %s.gz",filename);
                fprintf(stderr,ANSI_COLOR_YELLOW "Could not locate `%s' but found the gzip file `%s.gz'.\nRunning system command `" ANSI_COLOR_BLUE "%s"ANSI_COLOR_YELLOW"' now to uncompress"ANSI_COLOR_RESET "\n",filename,filename,buf);
                run_system_call(buf);
            }
        } else {
            //file exists -> nothing to do.
            fclose(fp);
        }
    }


    if(strncmp(format,"f",1)==0) { /*Read-in fast-food file*/
        //read fast-food file
        size_t bytes=sizeof(int) + sizeof(float)*9 + sizeof(int);//skip fdat
        bytes += sizeof(int) + sizeof(float) + sizeof(int); //skip znow
        int idat[5];
        FILE *fp = my_fopen(filename,"r");
        my_ftread(idat,sizeof(int),5,fp);
        np = (int64_t) idat[1]; //idat[1] is int.

        for(int i=0;i<num_fields;i++) {
            data[i] = my_malloc(size,np);
        }

        my_fseek(fp,bytes,SEEK_CUR);
        //Check that the file was written with the requested precision
        unsigned int dummy;
        my_fread(&dummy,sizeof(dummy), 1, fp);
        //so rewind by 4 bytes  prepare for calls to ftread
        my_fseek(fp, -sizeof(dummy), SEEK_CUR);
        dummy /= np;
        assert((dummy == 4 || dummy == 8) && "File must contain either 4 byte (float) or 8 byte(double) precision");

        if(dummy == size) {
            for(int i=0;i<num_fields;i++) {
                my_ftread(data[i],size, np, fp);
            }
        } else {
#ifndef SILENT
            fprintf(stderr,"WARNING: File was written in a different precision than requested (file precision = %u requested precision = %zu)\n",dummy,size);
#endif
            //Okay so the file was written in a different precision.
            //First, print a warning message and then read-in correctly with the
            //requested precision
            if(dummy == 4) {
                assert(size == 8 && "Expected to be storing to doubles");
                float *tmp = my_malloc(dummy,np);
                //read-in the fields
                for(int i=0;i<num_fields;i++) {
                    my_ftread(tmp, dummy, np, fp);
                    double *tmp_pos = (double *) data[i];
                    for(int64_t j=0;j<np;j++) tmp_pos[j] = tmp[j];
                }

                //free memory
                free(tmp);
            } else {
                assert(size == 4 && "Expected to be storing to doubles");
                double *tmp = my_malloc(dummy,np);

                //read-in the fields
                for(int i=0;i<num_fields;i++) {
                    my_ftread(tmp, dummy, np, fp);
                    float *tmp_pos = (float *) data[i];
                    for(int64_t j=0;j<np;j++) tmp_pos[j] = tmp[j];
                }
                //free memory
                free(tmp);
            }
        }

        fclose(fp);
    } else if(strncmp(format,"a",1)==0 || strncmp(format,"c",1)==0) { /* Read in ascii (white-space/comma) separated file*/
        int64_t i;
        int64_t nmax=300000;
        int nread;
        const int MAXBUFSIZE=10000;
        char buffer[MAXBUFSIZE];
        char delimiters[]=" ,\t";//delimiters are white-space, comma and tab

        for(i=0;i<num_fields;i++) {
            data[i] = my_malloc(size,nmax);
        }

        FILE *fp = my_fopen(filename,"r");
        i = 0 ;
        while(fgets(buffer,MAXBUFSIZE,fp) != NULL) {
            double tmp;
            char *token,*saveptr;
            int flag = 1;
            char *copy=buffer;
            for(int j=0;j<num_fields;j++,copy=NULL) {
                token = strtok_r(copy,delimiters,&saveptr);
                nread = sscanf(token,"%lf",&tmp);
                if(nread == 1) {
                    if(size==4) {
                        //data is float pointer
                        float *tmp_pos = (float *) data[j];
                        tmp_pos[i] = tmp;
                    } else {
                        //data is double pointer
                        double *tmp_pos = (double *) data[j];
                        tmp_pos[i] = tmp;
                    }
                } else {
                    flag = 0;
                }
            }
            if(flag == 1) {
                i++;
            } else {
                fprintf(stderr,ANSI_COLOR_YELLOW "io> WARNING: Could not parse all requested %d fields in line %"PRId64". Skipping that line" ANSI_COLOR_RESET "\n",num_fields,i);
            }

            if(i==nmax) {
                nmax *= MEMORY_INCREASE_FAC;
                while(nmax==i)
                    nmax += 5;

                for(int j=0;j<num_fields;j++) {
                    char varname[20];
                    snprintf(varname,20,"data[%d]",j);
                    data[j] = my_realloc(data[j],size,nmax,varname);
                }
            }
        }
        np=i ;
        nmax=np;

        //release the extra memory.
        for(int j=0;j<num_fields;j++) {
            char varname[20];
            snprintf(varname,20,"data[%d]",j);
            data[j] = my_realloc(data[j],size,nmax,varname);
        }
        fclose(fp);
    } else {
        fprintf(stderr,"ERROR: Unknown format `%s'\n",format);
        exit(EXIT_FAILURE);
    }

    va_list ap;
    va_start(ap,num_fields);

    assert(sizeof(void *) == sizeof(float *) && sizeof(void *) == sizeof(double *) && "Size of void pointer must be the same as size of float/double pointers");
    for(int i=0;i<num_fields;i++) {
        void **source = va_arg(ap, void **);
        *source =  data[i];
    }
    va_end(ap);

    return np;
}
