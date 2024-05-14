// xml_parser.h
#ifndef XML_PARSER_H
#define XML_PARSER_H

#include <vector>
#include <string>
#include <memory>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <string.h>

enum buffer_type {
    INPUT,
    OUTPUT,
    SCRATCH,
    BUFFER_UNKNOWN
};

enum op_type{
    SEND,
    RECV,
    REDUCE,
    RECV_REDUCE_COPY,
    RECV_COPY_SEND,
    RECV_REDUCE_SEND,
    RECV_REDUCE_COPY_SEND,
    OP_UNKNOWN
};


struct Step {
    int s;
    enum op_type type;
    enum buffer_type srcbuf;
    int srcoff;
    enum buffer_type dstbuf;
    int dstoff;
    int cnt;
    int depid;
    int deps;
    int hasdep;
};

struct ThreadBlock {
    int id;
    int send;
    int recv;
    int chan;
    std::vector<std::shared_ptr<Step>> steps;
};

struct Rank {
    int id;
    int i_chunks;
    int o_chunks;
    int s_chunks;
    std::vector<std::shared_ptr<ThreadBlock>> tbs;
};

class XMLParser {
public:
    void parseXMLAndFillStructs(const std::string& filename);
    void displayRanks(); // Utility function to display GPU information
    std::vector<std::shared_ptr<Rank>> ranks;
    int nchunksperloop;
private:
    void parseRank(xmlNode* node);
    void parseThreadBlock(xmlNode* node, std::shared_ptr<Rank>& rank);
    void parseStep(xmlNode* node, std::shared_ptr<ThreadBlock>& tb);
};

#endif // XML_PARSER_H
