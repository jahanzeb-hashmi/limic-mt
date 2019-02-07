/* Copyright (c) 2001-2016, The Ohio State University. All rights
 * reserved.
 *
 * This file is part of the MVAPICH2 software package developed by the
 * team members of The Ohio State University's Network-Based Computing
 * Laboratory (NBCL), headed by Professor Dhabaleswar K. (DK) Panda.
 *
 * For detailed copyright and licensing information, please refer to the
 * copyright file COPYRIGHT in the top level MVAPICH2 directory.
 *
 */

/*
 * limic.c
 *
 * LiMIC2:  Linux Kernel Module for High-Performance MPI Intra-Node
 *          Communication
 *
 * Author:  Hyun-Wook Jin <jinh@konkuk.ac.kr>
 *          System Software Laboratory
 *          Department of Computer Science and Engineering
 *          Konkuk University
 *
 * History: Jul 15 2007 Launch
 *
 *          Feb 27 2009 Modified by Karthik Gopalakrishnan (gopalakk@cse.ohio-state.edu)
 *                                  Jonathan Perkins       (perkinjo@cse.ohio-state.edu)
 *            - Add versioning data to the User Space Library
 */

#include <stdio.h>
#include <stdlib.h>
#include "limic.h"
#include "limic_internal.h"

#define requested_threads_default 4
#define pool_threshold_default 1048576 /* 1MB */

int limic_open( void )
{
    int fd, ret;
    uint32_t vinfo = ( (LIMIC_LIBRARY_MAJOR<<16) | LIMIC_LIBRARY_MINOR );

    fd = open("/dev/limic", O_RDONLY);
    if(fd == -1) {
        printf("LiMIC: (limic_open) file open fail\n");
        ret = -1;
        goto err_open;
    }

    ret = ioctl(fd, LIMIC_VERSION, &vinfo);
    if (ret != LIMIC_VERSION_OK) {
        printf("LiMIC: (limic_open) mismatch between module & library version\n");
        goto err_ioctl;
    }
    
    return fd;

err_ioctl:
    close(fd);

err_open:
    return ret;
}


void limic_close( int fd )
{
    close( fd );
}


size_t limic_tx_init( int fd, void *buf, size_t len, limic_user *lu )
{
    int ret;
    limic_request sreq;

    sreq.buf = buf;
    sreq.len = len;
    sreq.lu = lu;
    
    ret = ioctl(fd, LIMIC_TX, &sreq);
    if( ret != LIMIC_TX_DONE ){
        printf("LiMIC: (limic_tx_init) LIMIC_TX fail\n");
        return 0;
    }

    return len;
}


size_t limic_rx_comp( int fd, void *buf, size_t len, limic_user *lu )
{
    int ret;
    limic_request rreq;

    rreq.buf = buf;
    rreq.len = len;
    rreq.lu = lu;
    /* get environment variable for requested threads and assign */
    char *value = NULL;
    if ((value = getenv("MV2_LIMIC_KERN_THREADS")) != NULL) {
	    rreq.needed_workers = atoi(value);
    }
    else {
        /* default */
        rreq.needed_workers = requested_threads_default;
    }

    if ((value = getenv("MV2_LIMIC_POOL_MSG_THRESHOLD")) != NULL) {
    	rreq.pool_threshold = atoi(value);
    }
    else {
        /* default */
        rreq.needed_workers = requested_threads_default;
    }
    
    //printf ("[%d] Requesting %d threads from pool [pool threshold %d] \n", getpid(), rreq.needed_workers, rreq.pool_threshold);

    ret = ioctl(fd, LIMIC_RX, &rreq);
    if( ret != LIMIC_RX_DONE ){
        printf("LiMIC: (limic_rx_comp) LIMIC_RX fail\n");
        return 0;
    }

    return lu->length;
}

size_t limic_tx_comp( int fd, void *buf, size_t len, limic_user *lu )
{
    int ret;
    limic_request rreq;

    rreq.buf = buf;
    rreq.len = len;
    rreq.lu = lu;

    ret = ioctl(fd, LIMIC_TXW, &rreq);
    if( ret != LIMIC_TXW_DONE ){
        printf("LiMIC: (limic_tx_comp) LIMIC_TXW fail\n");
        return 0;
    }

    return lu->length;
}
