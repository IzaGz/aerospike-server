/*
 * hist.c
 *
 * Copyright (C) 2008-2014 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

#include "hist.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "citrusleaf/alloc.h"
#include "citrusleaf/cf_atomic.h"
#include "citrusleaf/cf_bits.h"
#include "citrusleaf/cf_clock.h"

#include "dynbuf.h"
#include "fault.h"


// #define DEBUG 1

histogram *
histogram_create(const char *name)
{
	histogram * h = cf_malloc(sizeof(histogram));
	if (!h)	return(0);
	if (strlen(name) >= sizeof(h->name)-1) { cf_free(h); return(0); }
	strcpy(h->name, name);
	h->n_counts = 0;
	memset(&h->count, 0, sizeof(h->count));
	return(h);
}

void histogram_clear(histogram *h)
{
	cf_atomic_int_set(&h->n_counts, 0);

	for (int i = 0; i < N_COUNTS; i++) {
		cf_atomic_int_set(&h->count[i], 0);
	}
}

void histogram_dump( histogram *h )
{
	char printbuf[100];
	int pos = 0; // location to print from
	printbuf[0] = '\0';

	cf_info(AS_INFO, "histogram dump: %s (%zu total)",h->name, h->n_counts);
	int i, j;
	int k = 0;
	for (j=N_COUNTS-1 ; j >= 0 ; j-- ) if (h->count[j]) break;
	for (i=0;i<N_COUNTS;i++) if (h->count[i]) break;
	for (; i<=j;i++) {
		if (h->count[i] > 0) { // print only non zero columns
			int bytes = sprintf((char *) (printbuf + pos), " (%02d: %010zu) ", i, h->count[i]);
			if (bytes <= 0)
			{
				cf_info(AS_INFO, "histogram printing error. Bailing ...");
				return;
			}
			pos += bytes;
			if (k % 4 == 3){
				 cf_info(AS_INFO, "%s", (char *) printbuf);
				 pos = 0;
				 printbuf[0] = '\0';
			}
			k++;
		}
	}
	if (pos > 0)
		cf_info(AS_INFO, "%s", (char *) printbuf);
}

#ifdef USE_CLOCK

void histogram_start( histogram *h, histogram_measure *hm)
{
	cf_atomic_int_incr(&h->n_counts);
//	clock_gettime( CLOCK_PROCESS_CPUTIME_ID, &hm->start);
	clock_gettime( CLOCK_MONOTONIC, &hm->start);
}

void histogram_stop(histogram *h, histogram_measure *hm)
{
	struct timespec now_ts;
//	clock_gettime( CLOCK_PROCESS_CPUTIME_ID, &now_ts);
	clock_gettime( CLOCK_MONOTONIC, &now_ts);
	uint64_t start = (hm->start.tv_sec * 1000000000L) + hm->start.tv_nsec;
	uint64_t now = (now_ts.tv_sec * 1000000000L) + now_ts.tv_nsec;
	uint64_t delta = now - start;

	int index = cf_bits_find_last_set_64(delta);
	if (index < 0) index = 0;

	cf_atomic_int_incr( &h->count[ index ] );

}

#endif // USE CLOCK


#ifdef USE_GETCYCLES

void histogram_start( histogram *h, histogram_measure *hm)
{
	cf_atomic_int_incr(&h->n_counts);
	hm->start = cf_getms();
//	hm->start = hist_getcycles();
}

void histogram_stop(histogram *h, histogram_measure *hm)
{
//	uint64_t delta = hist_getcycles() - hm->start;
	uint64_t delta = cf_getms() - hm->start;

	int index = cf_bits_find_last_set_64(delta);
	if (index < 0) index = 0;

	cf_atomic_int_incr( &h->count[ index ] );

}

void histogram_insert_delta( histogram *h, uint64_t delta)
{
	cf_atomic_int_incr(&h->n_counts);

	int index = cf_bits_find_last_set_64(delta);
	if (index < 0) index = 0;
	cf_atomic_int_incr( &h->count[ index ] );

}

void histogram_insert_data_point( histogram *h, uint64_t start)
{
	cf_atomic_int_incr(&h->n_counts);

	uint64_t end = cf_getms();
	uint64_t delta = end - start;

	int index = cf_bits_find_last_set_64(delta);
	if (index < 0) index = 0;
	if (start > end)
	{
		// Need to investigate why in some cases start is a couple of ms greater than end
		// Could it be rounding error (usually the difference is 1 but sometimes I have seen 2
		// cf_info(AS_INFO, "start = %"PRIu64" > end = %"PRIu64"", start, end);
		index = 0;
	}

	cf_atomic_int_incr( &h->count[ index ] );

}

#endif // USE_GETCYCLES


void histogram_get_counts(histogram *h, histogram_counts *hc)
{
	for (int i=0;i<N_COUNTS;i++)
		hc->count[i] = h->count[i];
	return;
}

linear_histogram *
linear_histogram_create(char *name, uint64_t start, uint64_t max_offset, int num_buckets)
{
	if (num_buckets > MAX_LINEAR_BUCKETS) {
		cf_crash(AS_INFO, "linear histogram num_buckets %u > max %u", num_buckets, MAX_LINEAR_BUCKETS);
	}

	linear_histogram * h = cf_malloc(sizeof(linear_histogram));
	if (!h)	return(0);
	if (0 != pthread_mutex_init(&h->info_lock, 0)) { cf_free(h); return(0); }
	h->info_snapshot[0] = 0;
	if (strlen(name) >= sizeof(h->name)-1) { cf_free(h); return(0); }
	strcpy(h->name, name);
	h->n_counts = 0;
	h->num_buckets = num_buckets;
	h->start = start;
	h->bucket_offset = max_offset / h->num_buckets;
	if (h->bucket_offset == 0) // avoid divide by zero while inserting data point
		h->bucket_offset = 1;
	memset(&h->count, 0, sizeof(h->count));
	return(h);
}

void linear_histogram_destroy(linear_histogram *h)
{
	pthread_mutex_destroy(&h->info_lock);
	cf_free(h);
}

// Note: not thread safe!
void linear_histogram_clear(linear_histogram *h, uint64_t start, uint64_t max_offset)
{
	h->n_counts = 0;
	h->start = start;
	h->bucket_offset = max_offset / h->num_buckets;
	if (h->bucket_offset == 0) // avoid divide by zero while inserting data point
		h->bucket_offset = 1;
	memset(&h->count, 0, sizeof(h->count));
}

void linear_histogram_insert_data_point( linear_histogram *h, uint64_t point)
{
	cf_atomic_int_incr(&h->n_counts);

	int64_t offset = point - h->start;
	int64_t index = 0;

	if (offset > 0) {
		index = offset / h->bucket_offset;

		if (index >= (int64_t)h->num_buckets) {
			index = h->num_buckets - 1;
		}
	}

	cf_atomic_int_incr( &h->count[ index ]);
}

void linear_histogram_get_counts(linear_histogram *h, linear_histogram_counts *hc)
{
	for (int i = 0; i < h->num_buckets; i++) {
		hc->count[i] = h->count[i];
	}
}

uint64_t linear_histogram_get_total(linear_histogram *h)
{
	return cf_atomic_int_get(h->n_counts);
}

// This routine is not thread safe and should be called from a single threaded routine
size_t linear_histogram_get_index_for_pct(linear_histogram *h, size_t pct)
{
	if (h->n_counts == 0)
		return 1;
	int min_limit = (h->n_counts * pct) / 100;
	if (min_limit >= h->n_counts)
		return h->num_buckets;
	int count = 0;
	for (int i = 0; i < h->num_buckets; i++) {
		count += h->count[i];
		if (count >= min_limit)
			return (i+1);
	}
	return h->num_buckets;
}

// Note: not thread safe!
bool linear_histogram_get_thresholds_for_fraction(linear_histogram* h, uint32_t tenths_pct, uint64_t* p_low, uint64_t* p_high, uint32_t* p_mid_tenths_pct)
{
	return linear_histogram_get_thresholds_for_subtotal(h, (h->n_counts * tenths_pct) / 1000, p_low, p_high, p_mid_tenths_pct);
}

// Note: not thread safe!
bool linear_histogram_get_thresholds_for_subtotal(linear_histogram* h, uint64_t subtotal, uint64_t* p_low, uint64_t* p_high, uint32_t* p_mid_tenths_pct)
{
	if (h->n_counts == 0) {
		*p_low = 0;
		*p_high = 0;
		*p_mid_tenths_pct = 0;
		return false;
	}

	uint64_t count = 0;
	int i;

	for (i = 0; i < h->num_buckets; i++) {
		count += h->count[i];

		if (count > subtotal) {
			break;
		}
	}

	if (i == h->num_buckets) {
		// This means subtotal >= h->n_counts.
		*p_low = 0;
		*p_high = 0;
		*p_mid_tenths_pct = 0;
		return true;
	}

	*p_low = h->start + (i * h->bucket_offset);
	*p_high = *p_low + h->bucket_offset;

	uint64_t bucket_subtotal = h->count[i] - (count - subtotal);

	// Round up to nearest tenth of a percent.
	*p_mid_tenths_pct = ((bucket_subtotal * 1000) + h->count[i] - 1) / h->count[i];

	return i == h->num_buckets - 1;
}

void linear_histogram_dump( linear_histogram *h )
{
	char printbuf[100];
	int pos = 0; // location to print from
	printbuf[0] = '\0';

	cf_debug(AS_NSUP, "linear histogram dump: %s [%u %u]/[%u] (%zu total)",
			h->name, h->start, h->start + h->num_buckets*h->bucket_offset, h->bucket_offset, h->n_counts);
	int i, j;
	int k = 0;
	for (j = h->num_buckets - 1; j >= 0; j--) if (h->count[j]) break;
	for (i = 0; i < h->num_buckets; i++) if (h->count[i]) break;
	for (; i<=j;i++) {
		if (h->count[i] > 0) { // print only non zero columns
			int bytes = sprintf((char *) (printbuf + pos), " (%02d: %010zu) ", i, h->count[i]);
			if (bytes <= 0)
			{
				cf_debug(AS_NSUP, "linear histogram printing error. Bailing ...");
				return;
			}
			pos += bytes;
			if (k % 4 == 3){
				 cf_debug(AS_NSUP, "%s", (char *) printbuf);
				 pos = 0;
				 printbuf[0] = '\0';
			}
			k++;
		}
	}
	if (pos > 0)
		cf_debug(AS_NSUP, "%s", (char *) printbuf);
}

void linear_histogram_save_info(linear_histogram *h)
{
	pthread_mutex_lock(&h->info_lock);

	// put in num buckets and the first count
	int idx = 0;
	int pos = snprintf(h->info_snapshot, INFO_SNAPSHOT_SIZE, "%d,%ld,%ld",
			h->num_buckets, h->bucket_offset, cf_atomic_int_get(h->count[idx++]));

	while (pos < INFO_SNAPSHOT_SIZE && idx < h->num_buckets) {
		pos += snprintf(h->info_snapshot + pos, INFO_SNAPSHOT_SIZE - pos,
				",%ld", h->count[idx++]);
	}

	pthread_mutex_unlock(&h->info_lock);
}

void linear_histogram_get_info(linear_histogram *h, cf_dyn_buf *db)
{
	pthread_mutex_lock(&h->info_lock);
	cf_dyn_buf_append_string(db, h->info_snapshot);
	pthread_mutex_unlock(&h->info_lock);
}
