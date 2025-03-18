###### Popescu Maria Bianca 335CA
# BitTorrent Protocol

## General Description

The tracker acts as a central coordinator. Its role is to register files and monitor the status of file segments held by each peer. To achieve this, I implemented functions such as **initializeTrackerState**, which initializes the tracker's state, and **handleInitialFileRegistration**, which processes initial registrations received from peers. Additionally, the tracker handles segment requests using the **handleFileRequest** function and updates swarm states through **handleUpdate**.

Each peer is responsible for downloading and uploading file segments. **download_thread_func** manages downloads, while **upload_thread_func handles** upload requests from other peers. Within **download_thread_func**, the peer requests a list of peers from the tracker using **requestPeerList**, identifies missing segments with **findFirstMissingSegment**, and downloads the optimal segments using **downloadSegment**. The upload thread, implemented in **upload_thread_func**, responds to segment requests using MPI.

The program flow begins with MPI initialization and determining the role of each process (tracker or peer). The tracker waits to receive initial registrations from all peers and then sends signals to start downloads. Peers, after registering and receiving the start signal, launch their download and upload threads, continuously interacting with the tracker to update their state and request necessary information.

For memory management, I implemented functions like **obtainPeerInformation** and **freePeerList**, which handle memory allocation and deallocation, preventing memory leaks. Additionally, retry mechanisms were implemented in downloadSegment to handle potential download failures.

Finally, once all downloads are complete, peers send a termination signal to the tracker via **finalizeDownload**, and the tracker notifies all processes to gracefully close connections and free allocated resources using **freeTracker**.
