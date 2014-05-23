#define NBR_ROUTER 5
#define MAXBUFLEN 1024


struct pkt_HELLO
{
	unsigned int router_id;
	unsigned int link_id;
};

struct pkt_LSPDU
{
	unsigned int sender;
	unsigned int router_id;
	unsigned int link_id;
	unsigned int cost;
	unsigned int via;
};

struct pkt_INIT
{
	unsigned int router_id;
};

struct link_cost
{
	unsigned int link;
	unsigned int cost;
};

struct circuit_DB
{
	unsigned int nbr_link;
	struct link_cost linkcost[NBR_ROUTER];
};

// A struct used to construct topology graph
// each node represents a certain router's information
struct topologyNode
{
	int router_id;
	struct circuit_DB circuitDB;   //use a circuitDB structure to record LSPDU
};

// A structure to record routing information
// each RIB contains one routing information to one router
struct RIB
{
	int router_id;
	int destination;
	int path;
	int cost;
	int inSetN;
};

void initStates( unsigned int *router_id, char **nse_host, char **nse_port, char **router_port, 
	             char const *argv[]);

void initTransferInfo(int *socket, struct addrinfo **sendToAddrInfo,
                      char *localPort, char *nse_host, char *nse_port);

void sendInitPkt(int localSocket, struct addrinfo *sendToAddrInfo, int router_id);

void receiveCircuitDB(struct circuit_DB *circuitDB, int localSocket);

void exchangePDUs(struct circuit_DB *circuitDB, int localSocket, struct addrinfo *sendToAddrInfo, 
	              unsigned int router_id, struct topologyNode *topologyGraph);

void sendHellos(struct circuit_DB *circuitDB, int localSocket, struct addrinfo *sendToAddrInfo, unsigned int router_id);

void receivePackets(struct circuit_DB *circuitDB, int localSocket, struct addrinfo *sendToAddrInfo, 
	                unsigned int router_id, struct topologyNode* topologyGraph, int *connected, struct RIB *rib);

void replyWithLSPDU(int localSocket, struct addrinfo *sendToAddrInfo, int router_id, struct topologyNode *topologyGraph, 
                    unsigned int link_id, unsigned int sender);
void printTopology(unsigned int router_id, struct topologyNode *topologyGraph, struct RIB* rib);

void printSentMsg(unsigned int sender, unsigned int router, int link, int cost, int via);
void printRecvMsg(unsigned int printer, unsigned int sender, unsigned router, int link, int cost, int via);
void processLSPDU(struct pkt_LSPDU *LSPDUpkt, int localSocket, struct addrinfo *sendToAddrInfo, int router_id, 
    struct topologyNode *topologyGraph, int *connected, struct RIB *rib);

void calculateRIB(int router_id, struct RIB *rib, struct topologyNode *topologyGraph, int * connected);