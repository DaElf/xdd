/* Copyright (C) 1992-2010 I/O Perforgance, Inc. and the
 * United States Departments of Energy (DoE) and Defense (DoD)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program in a file named 'Copying'; if not, write to
 * the Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139.
 */
/* Principal Author:
 *      Tom Ruwart (tmruwart@ioperformance.com)
 * Contributing Authors:
 *       Steve Hodson, DoE/ORNL
 *       Steve Poole, DoE/ORNL
 *       Bradly Settlemyer, DoE/ORNL
 *       Russell Cattelan, Digital Elves
 *       Alex Elder
 * Funding and resources provided by:
 * Oak Ridge National Labs, Department of Energy and Department of Defense
 *  Extreme Scale Systems Center ( ESSC ) http://www.csm.ornl.gov/essc/
 *  and the wonderful people at I/O Performance, Inc.
 */
/*
 * This file contains the subroutines used by target_pass() or targetpass_loop() 
 * that are specific to an End-to-End (E2E) operation.
 */
#include "xint.h"

/*----------------------------------------------------------------------------*/
/* xdd_targetpass_e2e_loop_dst() - This subroutine will manage assigning tasks to
 * Worker Threads during an E2E operation but only on the destination side of an
 * E2E operation. 
 * Called from xdd_targetpass()
 * 
 */
void
xdd_targetpass_e2e_loop_dst(xdd_plan_t* planp, target_data_t *tdp) {
	worker_data_t	*wdp;
	int		q;

	// The idea is to keep all the Worker Threads busy reading whatever is sent to them
	// from their respective Worker Threads on the Source Side.
	// The "normal" targetpass task assignment loop counts down the number
	// of bytes it has assigned to be transferred until the number of bytes remaining
	// becomes zero.
	// This task assignment loop is based on the availability of Worker Threads to perform
	// I/O tasks - namely a recvfrom/write task which is specific to the 
	// Destination Side of an E2E operation. Each Worker Thread will continue to perform
	// these recvfrom/write tasks until it receives an End-of-File (EOF) packet from 
	// the Source Side. At this point that Worker Thread remains "unavailable" but also 
	// turns on its "EOF" flag to indicate that it has received an EOF. It will also
	// enter the targetpass_worker_thread_passcomplete barrier so that by the time this 
	// subroutine returns, all the Worker Threads will be at the targetpass_worker_thread_passcomplete
	// barrier.
	//
	// This routine will keep requesting Worker Threads from the Worker Thread Locator until it
	// returns "zero" which indicates that all the Worker Threads have received an EOF.
	// At that point this routine will return.

	// Get the first Worker Thread pointer for this Target
	wdp = xdd_get_any_available_worker_thread(tdp);

	//////////////////////////// BEGIN I/O LOOP FOR ENTIRE PASS ///////////////////////////////////////////
	while (wdp) { 

		// Check to see if we've been canceled - if so, we need to leave this loop
		if ((xgp->canceled) || (xgp->abort) || (tdp->td_tgtstp->abort)) {
			// When we got this Worker Thread the WTSYNC_BUSY flag was set by get_any_available_worker_thread()
			// We need to reset it so that the subsequent loop will find it with get_specific_worker_thread()
			// Normally we would get the mutex lock to do this update but at this point it is not necessary.
			wdp->wd_worker_thread_target_sync &= ~WTSYNC_BUSY;
			break;
		}

	
		// Make sure the Worker Thread does not think the pass is complete
		wdp->wd_task_request = TASK_REQ_IO;
		tdp->td_tgtstp->my_current_op_type = OP_TYPE_WRITE;
		tdp->td_tgtstp->target_op_number = tdp->td_tgtstp->my_current_op_number;
		if (tdp->td_tgtstp->my_current_op_number == 0) 
			nclk_now(&tdp->td_tgtstp->my_first_op_start_time);

   		// If time stamping is on then assign a time stamp entry to this Worker Thread
   		if ((tdp->td_tsp->ts_options & (TS_ON|TS_TRIGGERED))) {
			wdp->wd_tsp->ts_current_entry = tdp->td_tsp->ts_current_entry;	
			tdp->td_tsp->ts_current_entry++;
			if (tdp->td_tsp->ts_options & TS_ONESHOT) { // Check to see if we are at the end of the ts buffer
				if (tdp->td_tsp->ts_current_entry == tdp->td_tsp->ts_size)
					tdp->td_tsp->ts_options &= ~TS_ON; // Turn off Time Stamping now that we are at the end of the time stamp buffer
			} else if (tdp->td_tsp->ts_options & TS_WRAP) {
				tdp->td_tsp->ts_current_entry = 0; // Wrap to the beginning of the time stamp buffer
			}
			wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].pass_number = tdp->td_tgtstp->my_current_pass_number;
			wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].worker_thread_number = wdp->wd_thread_number;
			wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].thread_id     = wdp->wd_thread_id;
			wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].op_type = OP_TYPE_WRITE;
			wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].op_number = -1; 		// to be filled in after data received
			wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].byte_location = -1; 	// to be filled in after data received
			wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].disk_xfer_size = 0; 	// to be filled in after data received
			wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].net_xfer_size = 0; 	// to be filled in after data received
		}

		// Release the Worker Thread to let it start working on this task
		xdd_barrier(&wdp->wd_thread_targetpass_wait_for_task_barrier,&tdp->td_occupant,0);
	
		tdp->td_tgtstp->my_current_op_number++;
		// Get another Worker Thread and lets keepit roling...
		wdp = xdd_get_any_available_worker_thread(tdp);
	
	} // End of WHILE loop
	//////////////////////////// END OF I/O LOOP FOR ENTIRE PASS ///////////////////////////////////////////
        
	// Check to see if we've been canceled - if so, we need to leave 
	if (xgp->canceled) {
		fprintf(xgp->errout,"\n%s: xdd_targetpass_e2e_loop_src: Target %d: ERROR: Canceled!\n",
			xgp->progname,
			tdp->td_target_number);
		return;
	}
	// Wait for all Worker Threads to complete their most recent task
	// The easiest way to do this is to get the Worker Thread pointer for each
	// Worker Thread specifically and then reset it's "busy" bit to 0.
	for (q = 0; q < tdp->td_queue_depth; q++) {
		wdp = xdd_get_specific_worker_thread(tdp,q);
		pthread_mutex_lock(&wdp->wd_worker_thread_target_sync_mutex);
		wdp->wd_worker_thread_target_sync &= ~WTSYNC_BUSY; // Mark this Worker Thread NOT Busy
		pthread_mutex_unlock(&wdp->wd_worker_thread_target_sync_mutex);
		// Check to see if we've been canceled - if so, we need to leave 
		if (xgp->canceled) {
			fprintf(xgp->errout,"\n%s: xdd_targetpass_e2e_loop_src: Target %d: ERROR: Canceled!\n",
				xgp->progname,
				tdp->td_target_number);
			break;
		}
	}

	if (tdp->td_tgtstp->my_current_io_status != 0) 
		planp->target_errno[tdp->td_target_number] = XDD_RETURN_VALUE_IOERROR;

	return;

} // End of xdd_targetpass_e2e_loop_dst()

/*----------------------------------------------------------------------------*/
/* xdd_targetpass_e2e_loop_src() - This subroutine will assign tasks to Worker Threads until
 * all bytes have been processed. It will then issue an End-of-Data Task to 
 * all Worker Threads one at a time. The End-of-Data Task will send an End-of-Data
 * packet to the Destination Side so that those Worker Threads know that there
 * is no more data to receive.
 * 
 * This subroutine is called by xdd_targetpass().
 */
void
xdd_targetpass_e2e_loop_src(xdd_plan_t* planp, target_data_t *tdp) {
	worker_data_t	*wdp;
	int		q;
	int32_t	status;


	while (tdp->td_bytes_remaining) {
		// Get pointer to next Worker Thread to issue a task to
		wdp = xdd_get_any_available_worker_thread(tdp);

		// Things to do before an I/O is issued
		status = xdd_target_ttd_before_io_op(tdp,wdp);
		// Check to see if either the pass or run time limit has expired - if so, we need to leave this loop
		if (status != XDD_RC_GOOD)
			break;

		// Set up the task for the Worker Thread
		xdd_targetpass_e2e_task_setup_src(wdp);

		// Update the pointers/counters in the Target PTDS to get ready for the next I/O operation
		tdp->td_tgtstp->my_current_byte_location += tdp->td_tgtstp->my_current_io_size;
		tdp->td_tgtstp->my_current_op_number++;
		tdp->td_bytes_issued += tdp->td_tgtstp->my_current_io_size;
		tdp->td_bytes_remaining -= tdp->td_tgtstp->my_current_io_size;

		// E2E Source Side needs to be monitored...
		if (tdp->td_target_options & TO_E2E_SOURCE_MONITOR)
			xdd_targetpass_e2e_monitor(tdp);

		// Release the Worker Thread to let it start working on this task
		xdd_barrier(&wdp->wd_thread_targetpass_wait_for_task_barrier,&tdp->td_occupant,0);

	} // End of WHILE loop that transfers data for a single pass

	// Check to see if we've been canceled - if so, we need to leave 
	if (xgp->canceled) {
		fprintf(xgp->errout,"\n%s: xdd_targetpass_e2e_loop_src: Target %d: ERROR: Canceled!\n",
			xgp->progname,
			tdp->td_target_number);
		return;
	}

	// Assign each of the Worker Threads an End-of-Data Task
	xdd_targetpass_e2e_eof_src(tdp);


	// Wait for all Worker Threads to complete their most recent task
	// The easiest way to do this is to get the Worker Thread pointer for each
	// Worker Thread specifically and then reset it's "busy" bit to 0.
	for (q = 0; q < tdp->td_queue_depth; q++) {
		wdp = xdd_get_specific_worker_thread(tdp,q);
		pthread_mutex_lock(&wdp->wd_worker_thread_target_sync_mutex);
		wdp->wd_worker_thread_target_sync &= ~WTSYNC_BUSY; // Mark this Worker Thread NOT Busy
		pthread_mutex_unlock(&wdp->wd_worker_thread_target_sync_mutex);
	}

	if (tdp->td_tgtstp->my_current_io_status != 0) 
		planp->target_errno[tdp->td_target_number] = XDD_RETURN_VALUE_IOERROR;

	return;

} // End of xdd_targetpass_e2e_loop_src()
/*----------------------------------------------------------------------------*/
/* xdd_targetpass_e2e_task_setup_src() - This subroutine will set up the task info for an I/O
 */
void
xdd_targetpass_e2e_task_setup_src(worker_data_t *wdp) {
	target_data_t	*tdp;

	tdp = wdp->wd_tdp;
	// Assign an IO task to this worker thread
	wdp->wd_task_request = TASK_REQ_IO;

	wdp->wd_e2ep->e2e_msg_sequence_number = tdp->td_e2ep->e2e_msg_sequence_number;
	tdp->td_e2ep->e2e_msg_sequence_number++;

	if (tdp->td_seekhdr.seeks[tdp->td_tgtstp->my_current_op_number].operation == SO_OP_READ) // READ Operation
		tdp->td_tgtstp->my_current_op_type = OP_TYPE_READ;
	else tdp->td_tgtstp->my_current_op_type = OP_TYPE_NOOP;

	// Figure out the transfer size to use for this I/O
	// It will be either the normal I/O size (tdp->td_iosize) or if this is the
	// end of this file then the last transfer could be less than the
	// normal I/O size. 
	if (tdp->td_bytes_remaining < tdp->td_iosize)
		tdp->td_tgtstp->my_current_io_size = tdp->td_bytes_remaining;
	else tdp->td_tgtstp->my_current_io_size = tdp->td_iosize;

	// Set the location to seek to 
	tdp->td_tgtstp->my_current_byte_location = tdp->td_tgtstp->my_current_byte_location;

	// Remember the operation number for this target
	tdp->td_tgtstp->target_op_number = tdp->td_tgtstp->my_current_op_number;
	if (tdp->td_tgtstp->my_current_op_number == 0) 
		nclk_now(&tdp->td_tgtstp->my_first_op_start_time);

   	// If time stamping is on then assign a time stamp entry to this Worker Thread
   	if ((tdp->td_tsp->ts_options & (TS_ON|TS_TRIGGERED))) {
		wdp->wd_tsp->ts_current_entry = tdp->td_tsp->ts_current_entry;	
		tdp->td_tsp->ts_current_entry++;
		if (tdp->td_tsp->ts_options & TS_ONESHOT) { // Check to see if we are at the end of the ts buffer
			if (tdp->td_tsp->ts_current_entry == tdp->td_tsp->ts_size)
				tdp->td_tsp->ts_options &= ~TS_ON; // Turn off Time Stamping now that we are at the end of the time stamp buffer
		} else if (tdp->td_tsp->ts_options & TS_WRAP) {
			tdp->td_tsp->ts_current_entry = 0; // Wrap to the beginning of the time stamp buffer
		}
		wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].pass_number = tdp->td_tgtstp->my_current_pass_number;
		wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].worker_thread_number = wdp->wd_thread_number;
		wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].thread_id     = wdp->wd_thread_id;
		wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].op_type = tdp->td_tgtstp->my_current_op_type;
		wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].op_number = tdp->td_tgtstp->target_op_number;
		wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].byte_location = tdp->td_tgtstp->my_current_byte_location;
	}

} // End of xdd_targetpass_e2e_task_setup_src()

/*----------------------------------------------------------------------------*/
/* xdd_targetpass_eof_source_side() - This subroutine will manage End-Of-File
 * processing for an End-to-End operation on the source side only.
 * This subroutine will cycle through all the Worker Threads for a specific Target Thread.
 * Upon completion of this routine all the Worker Threads on the SOURCE side will have
 * been given a task to send an EOF packet to their corresponding Worker Thread on the
 * Destination side.
 *
 * We need to issue an End-of-File task for this Worker Thread so that it sends an EOF
 * packet to the corresponding Worker Thread on the Destination Side. We do not have to wait for
 * that operation to complete. Just cycle through all the Worker Threads and later wait at the 
 * targetpass_worker_thread_passcomplete barrier. 
 *
 */
void
xdd_targetpass_e2e_eof_src(target_data_t *tdp) {
	worker_data_t	*wdp;
	int		q;

	for (q = 0; q < tdp->td_queue_depth; q++) {
		wdp = xdd_get_specific_worker_thread(tdp,q);
		wdp->wd_task_request = TASK_REQ_EOF;

   		// If time stamping is on then assign a time stamp entry to this Worker Thread
   		if ((tdp->td_tsp->ts_options & (TS_ON|TS_TRIGGERED))) {
			wdp->wd_tsp->ts_current_entry = tdp->td_tsp->ts_current_entry;	
			tdp->td_tsp->ts_current_entry++;
			if (tdp->td_tsp->ts_options & TS_ONESHOT) { // Check to see if we are at the end of the ts buffer
				if (tdp->td_tsp->ts_current_entry == tdp->td_tsp->ts_size)
					tdp->td_tsp->ts_options &= ~TS_ON; // Turn off Time Stamping now that we are at the end of the time stamp buffer
			} else if (tdp->td_tsp->ts_options & TS_WRAP) {
				tdp->td_tsp->ts_current_entry = 0; // Wrap to the beginning of the time stamp buffer
			}
		wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].pass_number = tdp->td_tgtstp->my_current_pass_number;
		wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].worker_thread_number = wdp->wd_thread_number;
		wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].thread_id     = wdp->wd_thread_id;
		wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].op_type = OP_TYPE_EOF;
		wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].op_number = -1*wdp->wd_thread_number;
		wdp->wd_ttp->tte[wdp->wd_tsp->ts_current_entry].byte_location = -1;
		}
	
		// Release the Worker Thread to let it start working on this task
		xdd_barrier(&wdp->wd_thread_targetpass_wait_for_task_barrier,&tdp->td_occupant,0);
	
	}
} // End of xdd_targetpass_eof_source_side()

/*----------------------------------------------------------------------------*/
/* xdd_targetpass_e2e_monitor() - This subroutine will monitor and display
 * information about the Worker Threads that are running on the Source Side of an
 * E2E operation.
 * 
 * This subroutine is called by xdd_targetpass_loop().
 */
void
xdd_targetpass_e2e_monitor(target_data_t *tdp) {
	worker_data_t	*tmpwdp;
	int qmax, qmin;
	int64_t opmax, opmin;
	int qavail;


	if ((tdp->td_tgtstp->my_current_op_number > 0) && ((tdp->td_tgtstp->my_current_op_number % tdp->td_queue_depth) == 0)) {
		qmin = 0;
		qmax = 0;
		opmin = tdp->td_target_ops;
		opmax = -1;
		qavail = 0;
		tmpwdp = tdp->td_next_wdp; // first Worker Thread on the chain
		while (tmpwdp) { // Scan the Worker Threads to determine the one furthest ahead and the one furthest behind
			if (tmpwdp->wd_worker_thread_target_sync & WTSYNC_BUSY) {
				if (tdp->td_tgtstp->target_op_number < opmin) {
					opmin = tdp->td_tgtstp->target_op_number;
					qmin = tmpwdp->wd_thread_number;
				}
				if (tdp->td_tgtstp->target_op_number > opmax) {
					opmax = tdp->td_tgtstp->target_op_number;
					qmax = tmpwdp->wd_thread_number;
				}
			} else {
				qavail++;
			}
			tmpwdp = tmpwdp->wd_next_wdp;
		}
		fprintf(stderr,"\n\nopmin %4lld, qmin %4d, opmax %4lld, qmax %4d, separation is %4lld, %4d worker threads busy, %lld percent complete\n\n",
			(long long int)opmin, qmin, (long long int)opmax, qmax, (long long int)(opmax-opmin+1), tdp->td_queue_depth-qavail, (long long int)((opmax*100)/tdp->td_target_ops));
	}
} // End of xdd_targetpass_e2e_monitor();

/*
 * Local variables:
 *  indent-tabs-mode: t
 *  default-tab-width: 4
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=4 sts=4 sw=4 noexpandtab
 */
