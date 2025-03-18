#include "tema2.h"

File* owned_files;
File* wanted_files;

File* obtainPeerInformation(int total_peers, File requested_file) {
    // alocam memorie pentru lista de peers
    File* peers_info = (File*)calloc(total_peers, sizeof(File));
    if (peers_info == NULL) {
        fprintf(stderr, "Error allocating memory for peers list\n");
        return NULL;
    }

    // initializam informatiile despre fiecare peer
    for (int peer_idx = 0; peer_idx < total_peers; peer_idx++) {
        peers_info[peer_idx].id = requested_file.id;
        peers_info[peer_idx].n_segments = requested_file.n_segments;
        
        // alocam memorie pentru segmente
        peers_info[peer_idx].segments = (char(*)[HASH_SIZE + 1])calloc(
            requested_file.n_segments, 
            sizeof(*(peers_info[peer_idx].segments))
        );
        
        // daca alocarea a esuat, eliberam memoria alocata anterior
        if (peers_info[peer_idx].segments == NULL) {
            for (int i = 0; i < peer_idx; i++) {
                free(peers_info[i].segments);
            }
            free(peers_info);
            fprintf(stderr, "Eroare la alocarea memoriei pentru segmente\n");
            return NULL;
        }
    }

    // primim mesajele de la tracker
    while (1) {
        int message_type;
        MPI_Recv(&message_type, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        
        if (message_type == 4) {
            break; // ne oprim cand tracker-ul trimite un semnal de finalizare
        }
        
        // primim informatiile despre un segment de la tracker
        int segment_index, peer_id;
        char segment_hash[HASH_SIZE + 1];
        
        MPI_Recv(&segment_index, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(&peer_id, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(segment_hash, HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, 
                 MPI_STATUS_IGNORE);
        
        // updatam informatiile despre segmentul primit
        strncpy(peers_info[peer_id].segments[segment_index], segment_hash, HASH_SIZE + 1);
    }

    return peers_info;
}

void* download_thread_func(void* arg) {
    ThreadContext* context = (ThreadContext*)arg;
    PeerState* state = context->state;
    
    int peer_id = state->rank;
    int num_files = state->num_wanted_files;
    int total_peers = state->num_tasks;
    
    // alocam si initializam matricea de utilizare a surselor
    int** source_usage = (int**)malloc((MAX_FILES + 1) * sizeof(int*));
    if (!source_usage) {
        fprintf(stderr, "Failed to allocate source_usage array\n");
        return NULL;
    }
    memset(source_usage, 0, (MAX_FILES + 1) * sizeof(int*));
    
    for (int i = 0; i <= MAX_FILES; i++) {
        source_usage[i] = (int*)malloc(total_peers * sizeof(int));
        if (!source_usage[i]) {
            fprintf(stderr, "Failed to allocate source_usage row %d\n", i);
            for (int j = 0; j < i; j++) {
                free(source_usage[j]);
            }
            free(source_usage);
            return NULL;
        }
        memset(source_usage[i], 0, total_peers * sizeof(int));
    }

    owned_files = state->owned_files;
    wanted_files = state->wanted_files;
    
    // ne plimbam prin toate fisierele pe care vrem sa le descarcam
    for (int file_idx = 0; file_idx < num_files; file_idx++) {
        int file_id = wanted_files[file_idx].id;
        int update_counter = 0;
        
        owned_files[file_id].id = file_id;
        File* peer_list = requestPeerList(file_id, total_peers); // cerem lista de peers
        if (!peer_list) continue;

        while (!isFileComplete(file_id)) { // cat timp nu am descarcat tot fisierul
            if (update_counter >= 10) { // updatam trackerul periodic
                updateTracker(file_id);
                peer_list = requestPeerList(file_id, total_peers);
                update_counter = 0;
            }

            // cautam primul segment lipsa
            int current_segment = findFirstMissingSegment(file_id); 
            if (current_segment == -1) break;

            // alegem cel mai bun peer din care sa descarcam
            int optimal_source = chooseOptimalSource(current_segment, peer_id, 
                                                   peer_list, source_usage[file_id], 
                                                   total_peers);
            if (optimal_source == -1) continue;

            // descarcam segmentul
            if (downloadSegment(optimal_source, current_segment, peer_list)) {
                source_usage[file_id][optimal_source]++;
                update_counter++;
            }
        }

        finalizeDownload(file_id, peer_id);
        freePeerList(peer_list, total_peers);
    }

    // trimitem un semnal catre tracker ca am terminat toate descarcarile
    int signal = 7;
    MPI_Send(&signal, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD);

    for (int i = 0; i <= MAX_FILES; i++) {
        free(source_usage[i]);
    }
    free(source_usage);
    return NULL;
}

File* requestPeerList(int file_id, int total_peers) {
    int signal = 2;
    // trimitem trackerului cererea de lista de peers
    MPI_Send(&signal, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD);
    MPI_Send(&file_id, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);
    
    int num_segments;
    // primim numarul de segmente din fisier
    MPI_Recv(&num_segments, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    owned_files[file_id].n_segments = num_segments;
    
    File temp_file = {.id = file_id, .n_segments = num_segments};
    return obtainPeerInformation(total_peers, temp_file);
}

void updateTracker(int file_id) {
    int signal = 5;
    // trimitem trackerului semnalul de update
    MPI_Send(&signal, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD);
    
    // trimitem informatiile despre segmentele pe care le detinem
    for (int i = 0; i < owned_files[file_id].n_segments; i++) {
        if (strlen(owned_files[file_id].segments[i]) > 0) {
            signal = 3;
            MPI_Send(&signal, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD);
            MPI_Send(&i, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);
            MPI_Send(&file_id, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);
            MPI_Send(owned_files[file_id].segments[i], HASH_SIZE + 1, MPI_CHAR, 
                    TRACKER_RANK, 0, MPI_COMM_WORLD);
        }
    }
    
    // trimitem semnalul de finalizare
    signal = 4;
    MPI_Send(&signal, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD);
}

int chooseOptimalSource(int segment, int peer_id, File* peer_list, int* usage, int total_peers) {
    int optimal_source = -1;
    int min_usage = INT_MAX;
    
    // cautam peer-ul cu cea mai mica utilizare
    for (int i = 1; i < total_peers; i++) {
        if (i != peer_id && strlen(peer_list[i].segments[segment]) > 0) {
            if (usage[i] < min_usage) {
                min_usage = usage[i];
                optimal_source = i;
            }
        }
    }
    
    return optimal_source;
}

bool downloadSegment(int source, int segment, File* peer_list) {
    int retry_count = 0;
    int signal;
    
    while (retry_count < MAX_RETRIES) {
        signal = 2;
        MPI_Send(&signal, 1, MPI_INT, source, 1, MPI_COMM_WORLD);
        MPI_Send(peer_list[source].segments[segment], HASH_SIZE + 1, MPI_CHAR, 
                 source, 1, MPI_COMM_WORLD);
        
        MPI_Recv(&signal, 1, MPI_INT, source, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        
        if (signal == 1) {
            // am primit segmentul cu succes
            strcpy(owned_files[peer_list[source].id].segments[segment], 
                   peer_list[source].segments[segment]);
            return true;
        } else {
            fprintf(stderr, "Retry %d for segment %d from peer %d\n", retry_count + 1, segment, source);
            retry_count++;
        }
    }
    
    fprintf(stderr, "Failed to download segment %d from peer %d after %d retries.\n",
            segment, source, MAX_RETRIES);
    
    return false;
}

void finalizeDownload(int file_id, int peer_id) {
    int signal = 6;
    // trimitem trackerului semnalul de finalizare a descarcarii
    MPI_Send(&signal, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD);
    MPI_Send(&file_id, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);
    
    char filename[MAX_FILENAME];
    sprintf(filename, "client%d_file%d", peer_id, file_id);
    
    FILE* file = fopen(filename, "w");
    if (file) {
        // scriem segmentele in fisier
        for (int i = 0; i < owned_files[file_id].n_segments; i++) {
            fprintf(file, "%s\n", owned_files[file_id].segments[i]);
        }
        fclose(file);
    }
}

void freePeerList(File* list, int total_peers) {
    for (int i = 0; i < total_peers; i++) {
        free(list[i].segments);
    }
    free(list);
}

bool isFileComplete(int file_id) {
    for (int i = 0; i < owned_files[file_id].n_segments; i++) {
        if (strlen(owned_files[file_id].segments[i]) == 0) {
            return false;
        }
    }
    return true;
}

int findFirstMissingSegment(int file_id) {
    for (int i = 0; i < owned_files[file_id].n_segments; i++) {
        if (strlen(owned_files[file_id].segments[i]) == 0) {
            return i;
        }
    }
    // daca nu gasim niciun segment lipsa
    return -1;
}

void* upload_thread_func(void* arg) {
    int peer_id = *(int*)arg;
    
    while (1) {
        MPI_Status status;
        int request_type = -1;
        
        // asteptam cereri de la alti clienti
        MPI_Recv(&request_type, 1, MPI_INT, MPI_ANY_SOURCE, 1, MPI_COMM_WORLD, &status);
        int requesting_peer = status.MPI_SOURCE;

        switch (request_type) {
            case 2: { // cerere de segment
                char requested_hash[HASH_SIZE + 1];
                MPI_Recv(requested_hash, HASH_SIZE + 1, MPI_CHAR, requesting_peer, 1, 
                        MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                
                int response = 1;
                MPI_Send(&response, 1, MPI_INT, requesting_peer, 0, MPI_COMM_WORLD);
                break;
            }
            
            case 7:
                return NULL;
                
            default:
                fprintf(stderr, "Peer %d received unexpected request type: %d\n", 
                        peer_id, request_type);
                break;
        }
    }

    return NULL;
}

TrackerState* initializeTrackerState(int num_tasks) {
    TrackerState* state = (TrackerState*)malloc(sizeof(TrackerState));
    if (!state) {
        fprintf(stderr, "Failed to allocate tracker state\n");
        return NULL;
    }
    
    state->num_tasks = num_tasks;
    state->active_clients = num_tasks - 1;

    // alocam memorie pentru matricea de swarms si seeds
    state->swarms = (int**)malloc(sizeof(int*) * (MAX_FILES + 1));
    if (!state->swarms) {
        fprintf(stderr, "Failed to allocate swarms array\n");
        free(state);
        return NULL;
    }
    
    state->seeds = (int**)malloc(sizeof(int*) * (MAX_FILES + 1));
    if (!state->seeds) {
        fprintf(stderr, "Failed to allocate seeds array\n");
        free(state->swarms);
        free(state);
        return NULL;
    }

    for (int i = 0; i < MAX_FILES + 1; i++) {
        state->swarms[i] = (int*)calloc(num_tasks, sizeof(int));
        state->seeds[i] = (int*)calloc(num_tasks, sizeof(int));
        if (!state->swarms[i] || !state->seeds[i]) {
            fprintf(stderr, "Failed to allocate matrices\n");
            for (int j = 0; j < i; j++) {
                free(state->swarms[j]);
                free(state->seeds[j]);
            }
            free(state->swarms);
            free(state->seeds);
            free(state);
            return NULL;
        }
    }

    // alocam memorie pentru matricea de fisiere
    state->all_files = (File**)malloc(sizeof(File*) * num_tasks);
    if (!state->all_files) {
        fprintf(stderr, "Failed to allocate all_files array\n");
        for (int i = 0; i < MAX_FILES + 1; i++) {
            free(state->swarms[i]);
            free(state->seeds[i]);
        }
        free(state->swarms);
        free(state->seeds);
        free(state);
        return NULL;
    }

    for (int i = 0; i < num_tasks; i++) {
        state->all_files[i] = (File*)malloc(sizeof(File) * (MAX_FILES + 1));
        if (!state->all_files[i]) {
            fprintf(stderr, "Failed to allocate files for peer %d\n", i);
            for (int j = 0; j < i; j++) {
                for (int k = 0; k < MAX_FILES + 1; k++) {
                    free(state->all_files[j][k].segments);
                }
                free(state->all_files[j]);
            }
            for (int j = 0; j < MAX_FILES + 1; j++) {
                free(state->swarms[j]);
                free(state->seeds[j]);
            }
            free(state->swarms);
            free(state->seeds);
            free(state->all_files);
            free(state);
            return NULL;
        }

        for (int j = 0; j < MAX_FILES + 1; j++) {
            state->all_files[i][j].n_segments = 0;
            // alocam memorie pentru segmente
            state->all_files[i][j].segments = (char(*)[HASH_SIZE + 1])malloc(
                MAX_CHUNKS * sizeof(*(state->all_files[i][j].segments)));
            if (!state->all_files[i][j].segments) {
                fprintf(stderr, "Failed to allocate segments for peer %d file %d\n", i, j);
                for (int k = 0; k < j; k++) {
                    free(state->all_files[i][k].segments);
                }
                for (int k = 0; k < i; k++) {
                    for (int l = 0; l < MAX_FILES + 1; l++) {
                        free(state->all_files[k][l].segments);
                    }
                    free(state->all_files[k]);
                }
                free(state->all_files[i]);
                for (int k = 0; k < MAX_FILES + 1; k++) {
                    free(state->swarms[k]);
                    free(state->seeds[k]);
                }
                free(state->swarms);
                free(state->seeds);
                free(state->all_files);
                free(state);
                return NULL;
            }

            // initializam segmentele cu șiruri goale
            for (int k = 0; k < MAX_CHUNKS; k++) {
                strcpy(state->all_files[i][j].segments[k], "");
            }
        }
    }

    return state;
}

void handleInitialFileRegistration(TrackerState* state) {
    for (int i = 0; i < state->num_tasks - 1; i++) {
        MPI_Status status;
        int sender_rank, num_files;

        // primim numarul de fisiere pe care le are
        MPI_Recv(&num_files, 1, MPI_INT, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);
        sender_rank = status.MPI_SOURCE;

        // primim informatiile despre fiecare fisier
        for (int j = 0; j < num_files; j++) {
            if (!receiveFileData(state, sender_rank)) {
                fprintf(stderr, "Eroare la recepția fișierului de la peer %d\n", sender_rank);
                continue;
            }
        }
    }
}

bool receiveFileData(TrackerState* state, int sender_rank) {
    File file;
    MPI_Recv(&file.id, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    state->swarms[file.id][sender_rank] = 1; // marcam peer-ul ca parte din swarm
    state->seeds[file.id][sender_rank] = 1; // marcam peer-ul ca seed

    MPI_Recv(&state->all_files[sender_rank][file.id].n_segments, 1, MPI_INT,
             sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    int segments = state->all_files[sender_rank][file.id].n_segments;

    for (int k = 1; k < state->num_tasks; k++) {
        state->all_files[k][file.id].n_segments = segments;
    }
    state->all_files[sender_rank][file.id].id = file.id;

    // primim datele despre fiecare segment
    for (int k = 0; k < segments; k++) {
        MPI_Recv(state->all_files[sender_rank][file.id].segments[k], HASH_SIZE + 1, MPI_CHAR,
                 sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }

    return true; // am primit cu succes datele despre fisier
}

void sendFileSegments(TrackerState* state, int peer_id, int file_id) {
    for (int i = 1; i < state->num_tasks; i++) {
        if (state->swarms[file_id][i] || state->seeds[file_id][i]) {
            int num_segments = state->all_files[i][file_id].n_segments;

            // trimitem informatiile despre fiecare segment
            for (int j = 0; j < num_segments; j++) {
                if (strlen(state->all_files[i][file_id].segments[j]) > 0) {
                    int signal = 3;
                    MPI_Send(&signal, 1, MPI_INT, peer_id, 0, MPI_COMM_WORLD);
                    MPI_Send(&j, 1, MPI_INT, peer_id, 0, MPI_COMM_WORLD);
                    MPI_Send(&i, 1, MPI_INT, peer_id, 0, MPI_COMM_WORLD);
                    MPI_Send(state->all_files[i][file_id].segments[j], 
                             HASH_SIZE + 1, MPI_CHAR, peer_id, 0, MPI_COMM_WORLD);
                }
            }
        }
    }

    // toate segmentele au fost trimise
    int signal = 4;
    MPI_Send(&signal, 1, MPI_INT, peer_id, 0, MPI_COMM_WORLD);
}

void handleFileRequest(TrackerState* state, int sender_rank) {
    int file_id;

    MPI_Recv(&file_id, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // trimitem numarul de segmente pe care trebuie sa le descarce
    for (int i = 1; i < state->num_tasks; i++) {
        if (state->swarms[file_id][i] == 1) {
            MPI_Send(&state->all_files[i][file_id].n_segments, 1, MPI_INT,
                     sender_rank, 0, MPI_COMM_WORLD);
            break;
        }
    }

    sendFileSegments(state, sender_rank, file_id);
}

void handleUpdate(TrackerState* state, int sender_rank) {
    while (1) {
        int signal;
        MPI_Recv(&signal, 1, MPI_INT, sender_rank, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        if (signal == 4) break;

        int segment_id, file_id;
        // primim informatiile despre segment
        MPI_Recv(&segment_id, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(&file_id, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        
        MPI_Recv(state->all_files[sender_rank][file_id].segments[segment_id], 
                HASH_SIZE + 1, MPI_CHAR, sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        
        state->swarms[file_id][sender_rank] = 1; // updatam informatiile despre swarm
    }
}

// copiaza segmentele unui fisier de la un peer la altul
void copyFileSegments(File* dest, File* src) {
    int segment_count = src->n_segments;
    dest->n_segments = segment_count;
    for (int j = 0; j < segment_count; j++) {
        memcpy(dest->segments[j], src->segments[j], HASH_SIZE + 1);
    }
}

void handleFileCompletion(TrackerState* state, int sender_rank) {
    int file_id;
    MPI_Recv(&file_id, 1, MPI_INT, sender_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    for (int i = 1; i < state->num_tasks; i++) {
        if (state->seeds[file_id][i]) {
            copyFileSegments(&state->all_files[sender_rank][file_id],
                             &state->all_files[i][file_id]);
            break;
        }
    }
    state->seeds[file_id][sender_rank] = 1; // marcam peer-ul ca seed
}

// eliberam memoria alocata pentru tracker
void freeTracker(TrackerState* state) {
    for (int i = 0; i < MAX_FILES + 1; i++) {
        free(state->swarms[i]);
        free(state->seeds[i]);
    }
    free(state->swarms);
    free(state->seeds);

    for (int i = 0; i < state->num_tasks; i++) {
        for (int j = 0; j < MAX_FILES + 1; j++) {
            free(state->all_files[i][j].segments);
        }
        free(state->all_files[i]);
    }
    free(state->all_files);
    free(state);
}

void tracker(int num_tasks, int rank) {
    TrackerState* state = initializeTrackerState(num_tasks);
    if (!state) {
        fprintf(stderr, "Failed to initialize tracker state\n");
        return;
    }

    handleInitialFileRegistration(state);

    int signal = 1; // semnal sa inceapa descarcarea
    for (int i = 1; i < num_tasks; i++) {
        MPI_Send(&signal, 1, MPI_INT, i, 0, MPI_COMM_WORLD);
    }

    while (state->active_clients > 0) {
        MPI_Status status;
        MPI_Recv(&signal, 1, MPI_INT, MPI_ANY_SOURCE, 1, MPI_COMM_WORLD, &status);
        int sender_rank = status.MPI_SOURCE;

        switch (signal) {
            case 2: // cerere de segment
                handleFileRequest(state, sender_rank);
                break;
            case 5: // update de la un peer
                handleUpdate(state, sender_rank);
                break;
            case 6: // completare descarcare
                handleFileCompletion(state, sender_rank);
                break;
            case 7: // terminare
                state->active_clients--;
                break;
        }
    }

    signal = 7;
    for (int i = 1; i < num_tasks; i++) {
        MPI_Send(&signal, 1, MPI_INT, i, 1, MPI_COMM_WORLD);
    }

    freeTracker(state);
}

void peer(int numtasks, int rank) {
    pthread_t threads[2];
    ThreadContext context;
    
    // initializam starea peer-ului
    PeerState state = {
        .rank = rank,
        .num_tasks = numtasks,
        .num_owned_files = 0,
        .num_wanted_files = 0,
        .owned_files = NULL,
        .wanted_files = NULL
    };
    
    if (!loadPeerConfiguration(&state, rank)) {
        cleanupPeerResources(&state);
        return;
    }
    
    if (!registerFilesWithTracker(&state)) {
        cleanupPeerResources(&state);
        return;
    }
    
    if (!waitForTrackerSignal()) {
        cleanupPeerResources(&state);
        return;
    }
    
    context.state = &state;
    context.should_terminate = false;
    
    if (!launchWorkerThreads(threads, &context)) {
        cleanupPeerResources(&state);
        return;
    }
    
    // asteptam ca thread-urile sa termine
    waitForThreadCompletion(threads);
    
    cleanupPeerResources(&state);
}

bool loadPeerConfiguration(PeerState* state, int rank) {
    char filename[MAX_FILENAME];
    sprintf(filename, "in%d.txt", rank);
    
    FILE* config = fopen(filename, "r");
    if (!config) return false;
    
    // citim numarul de fisiere detinute
    if (fscanf(config, "%d\n", &state->num_owned_files) != 1) {
        fclose(config);
        return false;
    }
    
    if (!initializeOwnedFiles(state)) {
        fclose(config);
        return false;
    }
    
    if (!parseOwnedFilesData(state, config)) {
        fclose(config);
        return false;
    }
    
    if (!parseWantedFilesData(state, config)) {
        fclose(config);
        return false;
    }
    
    fclose(config);
    return true;
}

bool initializeOwnedFiles(PeerState* state) {
    state->owned_files = calloc(MAX_FILES + 1, sizeof(File));
    if (!state->owned_files) return false;
    
    // aloca memorie pentru segmentele fiecarui fisier
    for (int i = 0; i <= MAX_FILES; i++) {
        state->owned_files[i].segments = calloc(MAX_CHUNKS, HASH_SIZE + 1);
        if (!state->owned_files[i].segments) {
            while (--i >= 0) free(state->owned_files[i].segments);
            free(state->owned_files);
            return false;
        }
    }
    
    return true;
}

bool parseOwnedFilesData(PeerState* state, FILE* config) {
    char buffer[256];
    
    for (int i = 0; i < state->num_owned_files; i++) {
        if (!fgets(buffer, sizeof(buffer), config)) return false;
        
        char filename[MAX_FILENAME];
        int segment_count;
        if (sscanf(buffer, "%s %d", filename, &segment_count) != 2) return false;
        
        int file_id = filename[strlen(filename) - 1] - '0';
        state->owned_files[file_id].id = file_id;
        state->owned_files[file_id].n_segments = segment_count;
        
        // citim hash-urile segmentelor
        for (int j = 0; j < segment_count; j++) {
            if (!fgets(buffer, sizeof(buffer), config)) return false;
            if (sscanf(buffer, "%s", state->owned_files[file_id].segments[j]) != 1) return false;
        }
    }
    
    return true;
}

bool parseWantedFilesData(PeerState* state, FILE* config) {
    // citim numarul de fisiere dorite
    if (fscanf(config, "%d\n", &state->num_wanted_files) != 1) return false;
    
    state->wanted_files = calloc(state->num_wanted_files, sizeof(File));
    if (!state->wanted_files) return false;
    
    char buffer[256];
    for (int i = 0; i < state->num_wanted_files; i++) {
        if (!fgets(buffer, sizeof(buffer), config)) return false;
        
        char filename[MAX_FILENAME];
        if (sscanf(buffer, "%s", filename) != 1) return false;
        
        state->wanted_files[i].id = filename[strlen(filename) - 1] - '0';
    }
    
    return true;
}

bool registerFilesWithTracker(PeerState* state) {
    // trimitem numarul de fisiere detinute tracker-ului
    if (MPI_Send(&state->num_owned_files, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD) != MPI_SUCCESS) {
        return false;
    }
    
    // trimitem informatiile despre fiecare fisier detinut
    for (int i = 1; i <= MAX_FILES; i++) {
        if (state->owned_files[i].n_segments == 0) continue;
        
        if (MPI_Send(&i, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD) != MPI_SUCCESS ||
            MPI_Send(&state->owned_files[i].n_segments, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD) != MPI_SUCCESS) {
            return false;
        }
        
        // trimitem fiecare segment
        for (int j = 0; j < state->owned_files[i].n_segments; j++) {
            if (MPI_Send(state->owned_files[i].segments[j], HASH_SIZE + 1, MPI_CHAR, 
                        TRACKER_RANK, 0, MPI_COMM_WORLD) != MPI_SUCCESS) {
                return false;
            }
        }
    }
    
    return true;
}

// asteapta trackerul sa trimita un semnal de incepere a descarcarii
bool waitForTrackerSignal(void) {
    int signal = -1;
    do {
        if (MPI_Recv(&signal, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, 
                     MPI_STATUS_IGNORE) != MPI_SUCCESS) {
            return false;
        }
    } while (signal != 1);
    return true;
}

// lanseaza cele doua thread-uri de download si upload
bool launchWorkerThreads(pthread_t threads[2], ThreadContext* context) {
    if (pthread_create(&threads[0], NULL, download_thread_func, context) != 0) {
        return false;
    }
    
    if (pthread_create(&threads[1], NULL, upload_thread_func, context) != 0) {
        pthread_cancel(threads[0]);
        return false;
    }
    
    return true;
}

void waitForThreadCompletion(pthread_t threads[2]) {
    for (int i = 0; i < 2; i++) {
        pthread_join(threads[i], NULL);
    }
}

void cleanupPeerResources(PeerState* state) {
    if (state->owned_files) {
        for (int i = 0; i <= MAX_FILES; i++) {
            free(state->owned_files[i].segments);
        }
        free(state->owned_files);
    }
    
    free(state->wanted_files);
}
 
int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();

}