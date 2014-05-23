#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <limits.h>
#include <netdb.h>
#include "router.h"

FILE *logFile;

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char const *argv[])
{
	// checkArgs(argc, argv[]);
	//-- arguments
	unsigned int router_id;
	char *nse_port;
	char *router_port;
	char *nse_host;
	// --
    // my structure to store topology
	struct topologyNode *topologyGraph = (struct topologyNode *)malloc(NBR_ROUTER * sizeof(struct topologyNode));
	memset(topologyGraph, 0, NBR_ROUTER*sizeof(topologyGraph));
	struct addrinfo *sendToAddrInfo;
	struct circuit_DB *circuitDB = (struct circuit_DB *)malloc(sizeof(struct circuit_DB));
	int local_socket;

    // Extract arguments from argv[], also create corresponding log file
	initStates(&router_id, &nse_host, &nse_port, &router_port, argv);
    // Initialize socket and prepare for transfer
	initTransferInfo(&local_socket, &sendToAddrInfo, router_port, nse_host, nse_port);

	sendInitPkt(local_socket, sendToAddrInfo, router_id);

	receiveCircuitDB(circuitDB, local_socket);

	exchangePDUs(circuitDB, local_socket, sendToAddrInfo, router_id, topologyGraph);

	free(topologyGraph);
	free(circuitDB);

    return 0;
}

/*
 * Send out the HELLO pkt and start exchanging PDUs
 */
void exchangePDUs(struct circuit_DB *circuitDB, int localSocket, struct addrinfo *sendToAddrInfo, 
                  unsigned int router_id, struct topologyNode *topologyGraph){
    struct RIB *rib = (struct RIB *)malloc((NBR_ROUTER+1) * sizeof(struct RIB));

    int *connected = (int *)malloc((NBR_ROUTER+1) * sizeof(int));  // indicate which link is connected
    for (int i = 1; i < (NBR_ROUTER+1); i++)                       // index is correspond to connected router_id
    {                         //When receiving HELLO packet from NSE, fill the link_id into the slots for record
        connected[i] = -1;    //initially non of the link is connected, and don't know which will be connected
    }

    //initialize the topology graph
    for (int j = 0; j < NBR_ROUTER; j++)
    {
        struct topologyNode *node = &(topologyGraph[j]);
        node->router_id = j+1;
        if (node->router_id == router_id)       //first put this router's info into topology topologygraph
        {
            memcpy((void *)&(node->circuitDB), (void *)circuitDB, sizeof(struct circuit_DB));
        } else {  // initialize the other nodes.
            (node->circuitDB).nbr_link = 0;    // Indicate that I don't have the PDUs for now
        }
    }

    sendHellos(circuitDB, localSocket, sendToAddrInfo, router_id);

    // Enter infinity loop for receive packets
    for ( ; ; ){
        receivePackets(circuitDB, localSocket, sendToAddrInfo, router_id, topologyGraph, connected, rib);
    }
    free(connected);
    free(rib);
}

/*
 * Used to receive packets
 */ 
void receivePackets(struct circuit_DB *circuitDB, int localSocket, struct addrinfo *sendToAddrInfo, 
	                unsigned int router_id, struct topologyNode* topologyGraph, int *connected, struct RIB *rib)
{
	//Simply use recvfrom() to receive packets
	struct addrinfo *remote_addr;
	socklen_t addr_len;
	char buf[MAXBUFLEN];
	int numOfBytes;
	addr_len = sizeof remote_addr;
    // Check if receive successed
    if ((numOfBytes = recvfrom(localSocket, buf, MAXBUFLEN , 0, (struct sockaddr *)&remote_addr, &addr_len)) == -1) {
        perror("recvfrom");
        exit(1);
    }
    buf[numOfBytes] = '\0';

    // check the packet type according to size
    if (numOfBytes == sizeof(struct pkt_HELLO))
    {
        struct pkt_HELLO helloPkt;
        memcpy(&helloPkt, buf, numOfBytes);
        connected[helloPkt.router_id] = helloPkt.link_id; //mark the router that sent the HELLO is connected
        fprintf(logFile,"R%d receives an HELLO packet: sender %d, via: %d\n", router_id, helloPkt.router_id, helloPkt.link_id);
        fflush(logFile);
    	replyWithLSPDU(localSocket, sendToAddrInfo, router_id, topologyGraph, helloPkt.link_id, router_id);
    } else if (numOfBytes == sizeof(struct pkt_LSPDU))
    {
        struct pkt_LSPDU *LSPDUpkt = (struct pkt_LSPDU *)malloc(sizeof(struct pkt_LSPDU));
        memcpy(LSPDUpkt, buf, numOfBytes);
        printRecvMsg(router_id, LSPDUpkt->sender, LSPDUpkt->router_id, LSPDUpkt->link_id, LSPDUpkt->cost, LSPDUpkt->via);
        processLSPDU(LSPDUpkt, localSocket, sendToAddrInfo, router_id, topologyGraph, connected, rib);
        free(LSPDUpkt);
    } else {
    	fprintf(logFile, "%s\n", "Wrong thing!!");
        fflush(logFile);
    }

    memset(&buf, 0, sizeof buf);         //clean the buff for next time use
}

// When receive a LSPDU, check if needed to update topology database.
// If so, update topology and re-calculate RIB. Also inform neighbours
void processLSPDU(struct pkt_LSPDU *LSPDUpkt, int localSocket, struct addrinfo *sendToAddrInfo, int router_id, 
    struct topologyNode *topologyGraph, int *connected, struct RIB *rib)
{
    // First check if need to update the topology, 
    // If not, just ignore, because we need to avoid loops
    struct topologyNode *node = &(topologyGraph[LSPDUpkt->router_id - 1]);

    if ((node->circuitDB).nbr_link <= 0)  //no entry record, definetly need to record it
    {
        //add the entry to topologyNode's circuitDB struct
        struct link_cost lc;
        lc.link = LSPDUpkt->link_id;
        lc.cost = LSPDUpkt->cost;
        memcpy(&(((node->circuitDB).linkcost)[0]), &lc, sizeof(struct link_cost));
        (node->circuitDB).nbr_link++;
        // Re-calculate shortest path here.
        calculateRIB(router_id, rib, topologyGraph, connected);
        printTopology(router_id, topologyGraph, rib);

        // send LSPDU to suitable neighbour
        struct topologyNode *node2 = &(topologyGraph[router_id-1]);   // find my neighbours and decide who should I send to
        int nbr_link = (node2->circuitDB).nbr_link;
        for (int i = 0; i < nbr_link; i++) 
        {   //for all the links connected to my neighbours
            int tmpLinkId = (((node2->circuitDB).linkcost)[i]).link;
            for (int j = 1 ; j < NBR_ROUTER+1; j++)
            {
                if (connected[j] == tmpLinkId && tmpLinkId != LSPDUpkt->via)   //this link is connected and not the sender link
                {
                    // Send the LSPDU
                    struct pkt_LSPDU data;
                    data.sender = router_id;
                    data.router_id = LSPDUpkt->router_id;
                    data.link_id = LSPDUpkt->link_id;
                    data.cost = LSPDUpkt->cost;
                    data.via = tmpLinkId;
                    int numOfBytes;
                    if ((numOfBytes = sendto(localSocket, &data, sizeof(struct pkt_LSPDU), 0,
                        sendToAddrInfo->ai_addr, sendToAddrInfo->ai_addrlen)) == -1) {
                        perror("router: sendto");
                        exit(1);
                    }
                    printSentMsg(router_id, LSPDUpkt->router_id, LSPDUpkt->link_id, LSPDUpkt->cost, tmpLinkId);
                }
            }
        }

    } else { // the record for the router is not empty, need to check if it's duplicated.
        int foundRecord = 0;
        for (int i = 0; i < (node->circuitDB).nbr_link; i++)  //loop through the topology to see if record need to be updated
        {
            struct link_cost lc = ((node->circuitDB).linkcost)[i];
            if (lc.link == LSPDUpkt->link_id && lc.cost == LSPDUpkt->cost)
            {
                foundRecord = 1;
                break;
            }
        }
        if (foundRecord !=1)
        {
            //update topology
            struct link_cost lc2;
            lc2.link = LSPDUpkt->link_id;
            lc2.cost = LSPDUpkt->cost;
            memcpy(&(((node->circuitDB).linkcost)[(node->circuitDB).nbr_link]), &lc2, sizeof(struct link_cost));
            (node->circuitDB).nbr_link++;
            calculateRIB(router_id, rib, topologyGraph, connected);
            printTopology(router_id, topologyGraph, rib);
            // send LSPDU to suitable neighbour
            struct topologyNode *node2 = &(topologyGraph[router_id-1]);   //read my own info to check whick link should send to
            int nbr_link = (node2->circuitDB).nbr_link;
            for (int i = 0; i < nbr_link; i++)
            {
                int tmpLinkId = (((node2->circuitDB).linkcost)[i]).link;
                for (int j = 1 ; j < NBR_ROUTER+1; j++)
                {
                    if (connected[j] == tmpLinkId && tmpLinkId != LSPDUpkt->via)   //this link is connected and not the sender link
                    {
                        // Send the LSPDU
                        struct pkt_LSPDU data;
                        data.sender = router_id;
                        data.router_id = LSPDUpkt->router_id;
                        data.link_id = LSPDUpkt->link_id;
                        data.cost = LSPDUpkt->cost;
                        data.via = tmpLinkId;
                        int numOfBytes;
                        if ((numOfBytes = sendto(localSocket, &data, sizeof(struct pkt_LSPDU), 0,
                            sendToAddrInfo->ai_addr, sendToAddrInfo->ai_addrlen)) == -1) {
                            perror("router: sendto");
                            exit(1);
                        }
                        printSentMsg(router_id, LSPDUpkt->router_id, LSPDUpkt->link_id, LSPDUpkt->cost, tmpLinkId);
                    }
                }
            }
        }
    }
}

// A sub-routine to print RIB according to different situation
void printRIB(struct RIB *rib, int router_id){
    fprintf(logFile, "# RIB\n");
    fflush(logFile);
    for (int i = 1; i < NBR_ROUTER+1; i++)
    {
        if (rib[i].cost == 0 && rib[i].inSetN == 1)
        {
            //printing the router itself
            fprintf(logFile, "R%d -> R%d -> Local, %d\n", router_id, router_id, rib[i].cost);
            fflush(logFile);
        }
        else if (rib[i].path == -1 && rib[i].inSetN == 1)
        {
            fprintf(logFile, "R%d -> R%d -> -, INFINITY\n", router_id, rib[i].destination);
            fflush(logFile);
        }
        else if(rib[i].inSetN == 1) {
            fprintf(logFile, "R%d -> R%d -> R%d, %d\n", router_id, rib[i].destination, rib[i].path, rib[i].cost);
            fflush(logFile);
        }
    }
}

// Sub-routine for dijkstra's algorithm to find the router with minimum cost value
int findMin(struct RIB *rib, struct topologyNode* topologyGraph){
    int min = INT_MAX;
    int minPos = 0;
    for (int i = 1; i < NBR_ROUTER+1; i++)
    {
        struct topologyNode *node = &(topologyGraph[i-1]);
        if (((node->circuitDB).nbr_link>0) && rib[i].inSetN == 0 && rib[i].cost <= min)  //Not in N and has minimun cost
        {                                                                                //Also need to make sure the router is in topology
            min = rib[i].cost;
            minPos = i;
        }
    }
    rib[minPos].inSetN = 1;
    return minPos;
}

// Sub-routine for dijkstra's algorithm match a link_id and router_id
int findRouter(struct topologyNode* topologyGraph, int router_id, int link_id){
    // printf("Link: %d\n", link_id);
    for (int i = 0; i < NBR_ROUTER; i++)
    {
        if (i != (router_id-1))   // two routers will have this link, we need the other side of the link
        {
            struct topologyNode *node = &(topologyGraph[i]);
            for (int j = 0; j < (node->circuitDB).nbr_link; j++)
            {
                struct link_cost lc = ((node->circuitDB).linkcost)[j];
                if (lc.link == link_id) // Has the target link, this is the router we looking for
                {
                    return topologyGraph[i].router_id;
                }
            }
        }
    }
    return -1;
}

/*
 * Calculate the RIB using dijkstra's algorithm
 */
void calculateRIB(int router_id, struct RIB *rib, struct topologyNode *topologyGraph, int *connected)
{
    int count = 1;
    for (int k = 1; k < NBR_ROUTER+1; k++)
    {// mark all infinity
        rib[k].router_id = router_id;
        rib[k].destination = k;
        rib[k].path = -1;       // which path to take
        rib[k].cost = INT_MAX;  // total cost to router
        rib[k].inSetN = 0;      // use to mark if router is in setN (dijkstra's )
    }   
    // put local info into RIB
    (rib[router_id]).router_id = router_id;
    (rib[router_id]).destination = router_id;
    (rib[router_id]).path = 0;
    (rib[router_id]).cost = 0;
    (rib[router_id]).inSetN = 1;

    struct topologyNode *node = &(topologyGraph[router_id-1]);
    for (int i = 1; i < NBR_ROUTER+1; i++)          //get the neighbour's cost value into D(dijkstra's )
    {
        if (connected[i]>0)
        {
            for (int j = 0; j < (node->circuitDB).nbr_link; j++)
            {
                struct link_cost lc = ((node->circuitDB).linkcost)[j];
                if (lc.link == connected[i])
                {
                    rib[i].cost = lc.cost;
                    rib[i].path = i;
                }
            }
        }
    }
    // loop processes of dijkstra's algorithm
    while(count < NBR_ROUTER){
        // find w not in N such that D(w) is a minimum
        int w = findMin(rib, topologyGraph);
        if (w == 0) // w=0 means i don't have other router's info. Just exit
        {
            return;
        }
        count++;
        struct topologyNode *wNode = &(topologyGraph[w-1]);
        // update D(v) for all v adjacent to w and not in N
        for (int a = 0; a < (wNode->circuitDB).nbr_link; a++)
        {
            struct link_cost lc = ((wNode->circuitDB).linkcost)[a];
            int dest = findRouter(topologyGraph, w, lc.link); //used to find v that is adjacent to w
            if ( (dest != -1) && (lc.cost+(rib[w].cost) < rib[dest].cost) && (rib[dest].inSetN == 0))
            {
                rib[dest].cost = lc.cost+(rib[w].cost);
                rib[dest].path = rib[w].path;
            }
        }
    }



}

/*
 * Reply the routers with all the LSPDUs that I have.
 */
 void replyWithLSPDU(int localSocket, struct addrinfo *sendToAddrInfo, int router_id, struct topologyNode *topologyGraph, 
      unsigned int link_id, unsigned int sender)
 {
    // Basically, loop through the topology and send out the LSPDU that I have
    for (int i = 0; i < NBR_ROUTER; i++)
    {
        struct topologyNode *node = &(topologyGraph[i]);
        int t_router_id = node->router_id;
        int nbr_link = (node->circuitDB).nbr_link;
        if (nbr_link > 0)
        {   // send out all the LSPDU that I have
            for (int j = 0; j < nbr_link; j++)
            {
                struct link_cost linkcost = ((node->circuitDB).linkcost)[j];
                struct pkt_LSPDU LSPDUpkt;
                LSPDUpkt.sender = sender;
                LSPDUpkt.router_id = t_router_id;
                LSPDUpkt.link_id = linkcost.link;
                LSPDUpkt.cost = linkcost.cost;
                LSPDUpkt.via = link_id;
                int numOfBytes;
                if ((numOfBytes = sendto(localSocket, &LSPDUpkt, sizeof(LSPDUpkt), 0,
                    sendToAddrInfo->ai_addr, sendToAddrInfo->ai_addrlen)) == -1) {
                    perror("router: sendto");
                    exit(1);
                }   
                printSentMsg(router_id, t_router_id, LSPDUpkt.link_id, LSPDUpkt.cost, link_id);
            }
        }
    }

 }

 void printTopology(unsigned int router_id, struct topologyNode *topologyGraph, struct RIB* rib)
 {
    fprintf(logFile, "%s\n", "# Topology database");
    fflush(logFile);
    for (int i = 0; i < NBR_ROUTER; i++)
    {
        struct topologyNode *node = &(topologyGraph[i]);
        int t_router_id = node->router_id;
        int nbr_link = (node->circuitDB).nbr_link;
        if (nbr_link > 0)
        {
            fprintf(logFile, "R%d -> R%d nbr link %d\n", router_id, t_router_id, nbr_link);
            fflush(logFile);
            for (int j = 0; j < nbr_link; j++)
            {
                struct link_cost lc = ((node->circuitDB).linkcost)[j];
                fprintf(logFile, "R%d -> R%d link %d cost %d\n", router_id, t_router_id, lc.link, lc.cost);
                fflush(logFile);
            }
        }
    }
    printRIB(rib, router_id);
 }



/*
 * Used as helper function send HELLO packets
 */
 void sendHellos(struct circuit_DB *circuitDB, int localSocket, struct addrinfo *sendToAddrInfo, unsigned int router_id)
 {
 	int numOfNbrs;
 	int numOfBytes;

 	numOfNbrs = circuitDB->nbr_link;
 	for (int i = 0; i < numOfNbrs; i++) //for all my neighbours
 	{
 		struct pkt_HELLO helloPkt;
 		helloPkt.router_id = router_id;
		helloPkt.link_id = ((circuitDB->linkcost)[i]).link;

		if ((numOfBytes = sendto(localSocket, &helloPkt, sizeof(helloPkt), 0,
             sendToAddrInfo->ai_addr, sendToAddrInfo->ai_addrlen)) == -1) {
        	perror("router: sendto");
        	exit(1);
    	}
        fprintf(logFile, "R%d sent an HELLO packet: sender %d, via %d\n", router_id, router_id, helloPkt.link_id);
        fflush(logFile);
 	}
 }

/*
 * Receive the circuit_DB from the nse
 */
void receiveCircuitDB(struct circuit_DB* circuitDB, int localSocket)
{
	struct addrinfo *remote_addr;
	socklen_t addr_len;
	char buf[MAXBUFLEN];
	int numOfBytes;

	addr_len = sizeof remote_addr;
    if ((numOfBytes = recvfrom(localSocket, buf, MAXBUFLEN , 0, (struct sockaddr *)&remote_addr, &addr_len)) == -1) {
        perror("recvfrom");
        exit(1);
    }
    buf[numOfBytes] = '\0';

    memcpy((void *)circuitDB, (const void *)buf, numOfBytes);
    memset(&buf, 0, sizeof buf);         //clean the buff for next time use
}

/*
 * Send out the Init packet to request for circuit_DB
 */
void sendInitPkt(int localSocket, struct addrinfo *sendToAddrInfo, int router_id){
	struct pkt_INIT initPkt;
	int numOfBytes;

    initPkt.router_id = router_id;

    if ((numOfBytes = sendto(localSocket, &initPkt, sizeof(initPkt), 0,
             sendToAddrInfo->ai_addr, sendToAddrInfo->ai_addrlen)) == -1) {
        perror("router: sendto");
        exit(1);
    }
    fprintf(logFile, "R%d sent an INIT packet: sender %d\n", router_id, router_id);
    fflush(logFile);

}

/*
 * Open a socket for datagram transfer.
 */
void initTransferInfo(int *t_socket, struct addrinfo **sendToAddrInfo, char *localPort, char *nse_host, char *nse_port){
	struct addrinfo hints, *serverInfo, *holder;
	int retVal;

	memset(&hints, 0, sizeof hints);    //Initialize the fields for getaddrinfo
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    // Get the local addr info, use it to create and bind local socket
    if ((retVal = getaddrinfo(NULL, localPort, &hints, &serverInfo)) != 0)
    {
    	fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(retVal));
        fflush(stderr);
        return;
    }
    // Use the first valid result to create socket
    for (holder = serverInfo; holder != NULL; holder = holder->ai_next)
    {
    	if ((*t_socket = socket(holder->ai_family, holder->ai_socktype, holder->ai_protocol)) == -1) {
            perror("router: socket");
            continue;
        }
        break;
    }
    // Need to check if socket is created successful
    if (holder == NULL)
    {
    	fprintf(stderr, "router: failed to bind socket\n");
        fflush(stderr);
        return;
    }

    //bind the local port to local host to receive msgs
    bind(*t_socket, holder->ai_addr, holder->ai_addrlen);

    // Now look up the remote addrinfo
    if ((retVal = getaddrinfo(nse_host, nse_port, &hints, &serverInfo)) != 0)
    {
    	fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(retVal));
        fflush(stderr);
        return;
    }
    *sendToAddrInfo = serverInfo;
}

// Change the type of arguments, also create corresponding log file
void initStates( unsigned int *router_id, char **nse_host, char **nse_port, char **router_port, 
	             char const *argv[])
{
	*router_id = atoi(argv[1]);
	*nse_host = (char*)argv[2];
	*nse_port = (char*)argv[3];
	*router_port = (char*)argv[4];

    char fileName[11+strlen(argv[1])];
    strcpy(fileName, "router");
    strcpy((fileName+6), argv[1]);
    strcpy((char *)((fileName+6)+strlen(argv[1])), ".log\0");
    logFile = fopen(fileName, "w+");

}

void printSentMsg(unsigned int sender, unsigned int router, int link, int cost, int via)
{
    fprintf(logFile, "R%d sent an LS PDU: sender %d, router_id %d, link_id %d, cost %d, via %d\n", sender, sender, router, link, cost, via);
    fflush(logFile);
}

void printRecvMsg(unsigned int printer, unsigned int sender, unsigned router, int link, int cost, int via)
{
    fprintf(logFile, "R%d receives an LS PDU: sender %d, router_id %d, link_id %d, cost %d, via %d\n", printer, sender, router, link, cost, via);
    fflush(logFile);
}


/*
 * CHeck if the arguements are valid;
 */
 void checkArgs(int argc, char const *argv[])
 {
    if (argc != 5)
    {
        printf("Check your arguments. More info in README.txt\n");
    }
 }