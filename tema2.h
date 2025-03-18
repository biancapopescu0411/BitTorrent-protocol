#ifndef TEMA2_H
#define TEMA2_H

#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100
#define MAX_RETRIES 5

typedef struct File {
    int id;
    int n_segments;
    char (*segments)[HASH_SIZE + 1];
} File;

typedef struct Peer_args {
    int rank;
    int n_files;
    int numtasks;
} Peer_args;

typedef struct TrackerState {
    File** all_files;
    int** swarms;
    int** seeds;
    int num_tasks;
    int active_clients;
} TrackerState;

typedef struct PeerState {
    int rank;
    int num_tasks;
    int num_owned_files;
    int num_wanted_files;
    File* owned_files;
    File* wanted_files;
} PeerState;

typedef struct ThreadContext {
    PeerState* state;
    bool should_terminate;
} ThreadContext;

File* obtainPeerInformation(int total_peers, File requested_file);
File* requestPeerList(int file_id, int total_peers);
void updateTracker(int file_id);
int chooseOptimalSource(int segment, int peer_id, File* peer_list, int* usage, int total_peers);
bool downloadSegment(int source, int segment, File* peer_list);
void finalizeDownload(int file_id, int peer_id);
void freePeerList(File* list, int total_peers);
bool isFileComplete(int file_id);
int findFirstMissingSegment(int file_id);
void* download_thread_func(void* arg);
void* upload_thread_func(void* arg);
void tracker(int numtasks, int rank);
void peer(int numtasks, int rank);
bool loadPeerConfiguration(PeerState* state, int rank);
bool initializeOwnedFiles(PeerState* state);
bool parseOwnedFilesData(PeerState* state, FILE* config);
bool parseWantedFilesData(PeerState* state, FILE* config);
bool registerFilesWithTracker(PeerState* state);
bool waitForTrackerSignal(void);
bool launchWorkerThreads(pthread_t threads[2], ThreadContext* context);
void waitForThreadCompletion(pthread_t threads[2]);
void cleanupPeerResources(PeerState* state);
bool receiveFileData(TrackerState* state, int sender_rank);
void copyFileSegments(File* dest, File* src);
void sendFileSegments(TrackerState* state, int peer_id, int file_id);

#endif