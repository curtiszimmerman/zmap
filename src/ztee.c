/*
* ZTee Copyright 2014 Regents of the University of Michigan
*
* Licensed under the Apache License, Version 2.0 (the "License"); you may not
* use this file except in compliance with the License. You may obtain a copy
* of the License at http://www.apache.org/licenses/LICENSE-2.0
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <errno.h>
#include <getopt.h>
#include <pthread.h>

#include "../lib/lockfd.h"
#include "../lib/logger.h"
#include "../lib/queue.h"
#include "../lib/util.h"
#include "../lib/xalloc.h"

#include "topt.h"

char *output_filename = NULL;
char *monitor_filename = NULL;
FILE *output_file = NULL;
FILE *status_updates_file = NULL;

pthread_mutex_t queue_size_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lock_queue = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t read_in_waiting = PTHREAD_COND_INITIALIZER;

//keeps track if input file is csv
int input_csv = 0;
//keeps track if output file is csv
int output_csv = 0;
//set to 1 if this flag is set
int find_success_only = 0;
//keep track of what field the is_successful field is
int success_field = 0;
//tells whether the is_successful field has been found or not
int success_found = 0;
//keeps track of the number of fields present in csv (minus 1 because 0 index)
int number_of_fields = 0;
int ip_field = 0;
int ip_field_found = 0;
int monitor = 0;
int done = 0;
int process_done = 0;
int total_read_in = 0;
int read_in_last_sec = 0;

double start_time;

pthread_t threads[3];

//one thread reads in
//one thread writes out and parses

//pops next element and determines what to do
//if zqueue_t is empty and read_in is finished, then
//it exits
void *process_queue (void* my_q);

void print_error () __attribute__ ((noreturn));

//uses fgets to read from stdin and add it to the zqueue_t
void *read_in (void* my_q);

//does the same as find UP but finds only successful IPs, determined by the
//is_successful field and flag
void find_successful_IP (char* my_string);

//finds IP in the string of csv and sends it to stdout for zgrab
//you need to know what position is the csv string the ip field is in
//zero indexed
void find_IP (char* my_string);

//writes a csv string out to csv file
//fprintf(stderr, "Is empty inside if %i\n", is_empty(my_queue));
void write_out_to_file (char* data);

//figure out how many fields are present if it is a csv
void figure_out_fields (char* data);

//checks if input is csv or just ip
//only needs to be run one time and on the first znode_t
//because the first znode_t will have the different fields or
//just the ip address`
int input_is_csv (zqueue_t *my_queue);

//check that the output file is either in a csv form or json form
//throws error is it is not either
//NOTE: JSON OUTPUT NOT IMPLEMENTED
void output_file_is_csv ();

void print_thread_error ();

//monitor code for ztee
//executes every second
void *monitor_ztee(void *my_q);

#define SET_IF_GIVEN(DST,ARG) \
	{ if (args.ARG##_given) { (DST) = args.ARG##_arg; }; }
#define SET_BOOL(DST,ARG) \
	{ if (args.ARG##_given) { (DST) = 1; }; }

int main(int argc, char *argv[])
{
	struct gengetopt_args_info args;
	struct cmdline_parser_params *params;
	params = cmdline_parser_params_create();
	assert(params);
	params->initialize = 1;
	params->override = 0;
	params->check_required = 0;

	if (cmdline_parser_ext(argc, argv, &args, params) != 0) {
		exit(EXIT_SUCCESS);
	}

	log_init(stderr, ZLOG_WARN, 0, NULL);

	// Handle help text and version
	if (args.help_given) {
		cmdline_parser_print_help();
		exit(EXIT_SUCCESS);
	}
	if (args.version_given) {
		cmdline_parser_print_version();
		exit(EXIT_SUCCESS);
	}

	// Check for an output file
	if (args.inputs_num < 1) {
		log_fatal("ztee", "No output file specified");
	}
	if (args.inputs_num > 1) {
		log_fatal("ztee", "Extra positional arguments starting with %s",
				args.inputs[1]);
	}

	output_filename = args.inputs[0];
	output_file = fopen(output_filename, "w");
	if (!output_file) {
		log_fatal("ztee", "Could not open output file %s, %s",
				output_filename, strerror(errno));
	}

	// Read actual options
	SET_BOOL(find_success_only, success_only);
	SET_BOOL(monitor, monitor);

	// Open the status update file if necessary
	if (args.status_updates_file_given) {
		monitor_filename = args.status_updates_file_arg;
		status_updates_file = fopen(monitor_filename, "w");
		if (!status_updates_file) {
			log_fatal("Unable to open monitor file %s, %s",
					monitor_filename, strerror(errno));
		}
	}

	zqueue_t* my_queue;
	my_queue = queue_init();
	int y = pthread_create(&threads[0], NULL, read_in, my_queue);
	const char* read = "read thread\n";

	if (y) {
		print_thread_error(read);
		exit(1);
	}

	int returned = input_is_csv(my_queue);

	if (returned) {
		return 0;
	}

	start_time = now();

	int a = pthread_create(&threads[1], NULL, process_queue, my_queue);
	const char* process = "process thread\n";

	if (a) {
		print_thread_error(process);
		exit(1);
	}

	if (monitor || status_updates_file) {
		int z = pthread_create(&threads[2], NULL, monitor_ztee, my_queue);
		const char* monitor_thread = "monitor thread\n";
		if(z){
			print_thread_error(monitor_thread);
			exit(1);
		}
	}

	int g;
	for(g=0; g < 3; g++){
		pthread_join(threads[g], NULL);
	}

}

void *process_queue(void* my_q)
{
	zqueue_t *my_queue = (zqueue_t *)my_q;

	while (!is_empty(my_queue) || !done){

		znode_t* temp = malloc(sizeof(znode_t));

		pthread_mutex_lock(&lock_queue);
		check_queue(my_queue);
		while (!done && is_empty(my_queue)) {
			pthread_cond_wait(&read_in_waiting, &lock_queue);
		}

		if (done && is_empty(my_queue)) {
			process_done = 1;
			fflush(output_file);
			fclose(output_file);
			free(my_queue);
			return NULL;
		}

		if (!is_empty(my_queue)) {
			temp = pop_front(my_queue);
		}

		pthread_mutex_unlock(&lock_queue);

		if (!input_csv) {
			fprintf(stdout, "%s", temp->data);
		}else if (find_success_only) {
			find_successful_IP(temp->data);
		}else {
			find_IP(temp->data);
		}
		write_out_to_file(temp->data);
		free(temp);
	}
	process_done = 1;
	fflush(output_file);
	fclose(output_file);
	return NULL;
}

void print_error()
{
	//includes incorrect output file format
	//
	printf("Problem with file format\n");
	exit(0);
}

void *read_in(void* my_q)
{
	//read in from stdin and add to back of linked list

	char* input = NULL;
	size_t length;
	zqueue_t *my_queue = (zqueue_t*)my_q;
	input = malloc(sizeof(char) *1000);

	//geline
	while (getline(&input, &length, stdin) > 0) {
		pthread_mutex_lock(&lock_queue);
		push_back(input, my_queue);
		check_queue(my_queue);
		pthread_mutex_unlock(&lock_queue);

		pthread_mutex_lock(&queue_size_lock);
		total_read_in++;
		read_in_last_sec++;
		pthread_mutex_unlock(&queue_size_lock);
	}

	pthread_mutex_lock(&lock_queue);
	done = 1;
	pthread_cond_signal(&read_in_waiting);
	pthread_mutex_unlock(&lock_queue);
	pthread_exit(NULL);
	return NULL;
}

void find_successful_IP(char* my_string)
{

	char *found;
	char *new_found;
	char *this_IP;
	int length;
	int i;
	char* is_this_IP_successful = 0;
	int is_successful = 0;
	found = strdup(my_string);
	new_found = strchr(found, ',');

	for (i=0; i <= number_of_fields; i++) {
		if (i == success_field && new_found) {
			length = strlen(found) - strlen(new_found);
			is_this_IP_successful = malloc(sizeof(char*)*(length+1));
			strncpy(is_this_IP_successful, found, length);
			is_successful = atoi(is_this_IP_successful);
			if(!is_successful) return;
		} else if(i == success_field) {
			is_successful = atoi( found );
			if(!is_successful) return;
		}

		if (i == ip_field && new_found) {
			length = strlen(found) - strlen(new_found);
			this_IP = malloc(sizeof(char*)*(length+1));
			strncpy(this_IP, found, length);
			this_IP[length] = '\0';
		} else if(i == ip_field) {
			this_IP = found;
		}

		if (new_found) found = new_found+1;
		new_found = strchr(found, ',');
	}
}

void find_IP(char* my_string)
{
	//finds IP in the string of csv and sends it to stdout for zgrab
	//you need to know what position is the csv string the ip field is in
	//zero indexed
	char *found;
	char *new_found;
	char *temp = NULL;
	int length;
	int i;
	found = strdup(my_string);
	new_found = strchr(found, ',');

	for (i = 0; i <= number_of_fields; i++) {
		if (i == ip_field && new_found) {
			temp = NULL;
			length = strlen(found) - strlen(new_found);
			temp = malloc(sizeof(char*)*(length+1));
			strncpy(temp, found, length);
			temp[length] = '\0';
			fprintf(stdout, "%s\n", temp);
			return;
		} else if(i == ip_field) {
			fprintf(stdout, "%s\n", found);
			return;
		}
		if(new_found) found = new_found+1;
		new_found = strchr(found, ',');

	}
	fprintf(stdout, "%s\n", temp);
}

void write_out_to_file(char* data)
{
	//take whatever is in the front of the linked list and parse it out to the
	//outputfile
	fprintf(output_file, "%s", data);
}

void figure_out_fields(char* data)
{
	//number_of_fields = number of commas + 1
	//check each substring if it is the same as saddr
	//set ip_field
	char *temp;
	const char* saddr = "input saddr";
	char* found;
	const char* is_successful = "success\0";
	char *new_found;
	int count = 0;
	int length;
	found = data;
	new_found = strchr(found, ',');

	while (new_found) {
		if (!ip_field_found) {
			length = strlen(found) - strlen(new_found);
			temp = NULL;
			temp = malloc(sizeof(char*)*(length+1));
			strncpy(temp, found, length);

			if (!strncmp(temp, saddr, 5)) {
				ip_field = count;
				ip_field_found = 1;
			}
		}

		if (!success_found) {
			if (!strncmp(temp, is_successful, 7)) {
				success_found = 1;
				success_field = count;
			}
		}
		found = new_found+2;
		new_found = NULL;
		count++;
		new_found = strchr(found, ',');
	}

	number_of_fields = count++;
	if (!ip_field_found) {
		if (!strncmp(found, saddr, 5)) {
			ip_field = number_of_fields;
			ip_field_found = 1;
		}
	}
	if (!success_found) {
		if (!strncmp(found, is_successful, 7)) {
			success_found = 1;
			success_field = number_of_fields;
		}
	}
}

int input_is_csv(zqueue_t *my_queue)
{
	//checks if input is csv or just ip
	//only needs to be run one time and on the first znode_t
	//because the first znode_t will have the different fields or
	//just the ip address`
	while (is_empty(my_queue) && !done);
	if (is_empty(my_queue) && done) {
		return 1;
	}

	znode_t *temp = malloc(sizeof(znode_t));
	temp = get_front(my_queue);

	char *found;
	found = strchr(temp->data, ',');
	if (!found) {
		input_csv = 0;
	}else {
		znode_t *to_delete = malloc(sizeof(znode_t*));
		input_csv = 1;
		to_delete = pop_front(my_queue);
		figure_out_fields(temp->data);
		fprintf(output_file, "%s", temp->data);
		free(to_delete);
	}
	output_file_is_csv();
	return 0;
}

void output_file_is_csv()
{
	int length = strlen(output_filename);
	char *end_of_file = malloc(sizeof(char*) *4);
	strncpy(end_of_file, output_filename+(length - 3), 3);
	end_of_file[4] = '\0';
	const char *csv = "csv\n";
	const char *json = "jso\n";
	if(!strncmp(end_of_file, csv, 3) && !strncmp(end_of_file, json, 3)){
		print_error();
	}
	if(!strncmp(end_of_file, csv, 3)) output_csv = 1;
	if(!strncmp(end_of_file, json, 3)) output_csv = 0;
}

void print_thread_error(char* string)
{
	fprintf(stderr, "Could not create thread %s\n", string);
	return;
}

#define TIME_STR_LEN 20

typedef struct ztee_stats {
	// Read stats
	uint32_t total_read;
	uint32_t read_per_sec_avg;
	uint32_t read_last_sec;

	// Buffer stats
	uint32_t buffer_cur_size;
	uint32_t buffer_avg_size;
	uint64_t _buffer_size_sum;

	// Duration
	double _last_age;
	uint32_t time_past;
	char time_past_str[TIME_STR_LEN];
} stats_t;

void update_stats(stats_t *stats, zqueue_t *queue)
{
	double age = now() - start_time;
	double delta = age - stats->_last_age;
	stats->_last_age = age;

	stats->time_past = age;
	time_string((int)age, 0, stats->time_past_str, TIME_STR_LEN);

	uint32_t total_read = total_read_in;
	stats->read_last_sec = (total_read - stats->total_read) / delta;
	stats->total_read = total_read;
	stats->read_per_sec_avg = stats->total_read / age;

	stats->buffer_cur_size = get_size(queue);
	stats->_buffer_size_sum += stats->buffer_cur_size;
	stats->buffer_avg_size = stats->_buffer_size_sum / age;
}

void *monitor_ztee(void* arg)
{
	zqueue_t *queue = (zqueue_t *) arg;
	stats_t *stats = xmalloc(sizeof(stats_t));

	if (status_updates_file) {
		fprintf(status_updates_file,"time_past,total_read_in,read_in_last_sec,read_per_sec_avg,buffer_current_size,buffer_avg_size\n");
		fflush(status_updates_file);
	}
	while (!process_done) {
		sleep(1);

		update_stats(stats, queue);
		if (monitor) {
			lock_file(stderr);
			fprintf(stderr, "%5s read_rate: %u rows/s (avg %u rows/s), buffer_size: %u (avg %u)\n",
					stats->time_past_str,
					stats->read_last_sec,
					stats->read_per_sec_avg,
					stats->buffer_cur_size,
					stats->buffer_avg_size);
			fflush(stderr);
			unlock_file(stderr);
		}
		if (status_updates_file) {
			fprintf(status_updates_file, "%u,%u,%u,%u,%u,%u\n",
					stats->time_past,
					stats->total_read,
					stats->read_last_sec,
					stats->read_per_sec_avg,
					stats->buffer_cur_size,
					stats->buffer_avg_size);
			fflush(status_updates_file);
		}
	}
	if (monitor) {
		lock_file(stderr);
		fflush(stderr);
		unlock_file(stderr);
	}
	if (status_updates_file) {
		fflush(status_updates_file);
		fclose(status_updates_file);
	}
	return NULL;
}
