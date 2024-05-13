// xml_parser.cc
#include "xml_parser.h"
#include <iostream>
#include <cstdlib>

enum op_type op_string_to_enmu(std::string string){
    if (string == "s") {
        return op_type::SEND;
    } else if (string == "r") {
        return op_type::RECV;
    } else if (string == "re") {
        return op_type::REDUCE;
    } else if (string == "rrc") {
        return op_type::RECV_REDUCE_COPY;
    } else if (string == "rrcs") {
        return op_type::RECV_REDUCE_COPY_SEND;
    }
    return op_type::OP_UNKNOWN;
}

enum buffer_type buffer_string_to_enmu(std::string string){
    if (string == "i") {
        return buffer_type::INPUT;
    } else if (string == "o") {
        return buffer_type::OUTPUT;
    } else if (string == "s") {
        return buffer_type::SCRATCH;
    }
    return buffer_type::BUFFER_UNKNOWN;
}

void XMLParser::parseXMLAndFillStructs(const std::string& filename) {
    xmlDocPtr doc = xmlReadFile(filename.c_str(), NULL, 0);
    if (!doc) {
        std::cerr << "Failed to parse XML file " << filename << std::endl;
        return;
    }

    xmlNode* root_element = xmlDocGetRootElement(doc);

    for (xmlNode* node = root_element->children; node; node = node->next) {
        if (node->type == XML_ELEMENT_NODE && strcmp((const char*)node->name, "gpu") == 0) {
            parseRank(node);
        }
    }

    xmlFreeDoc(doc);
    xmlCleanupParser();
}

void XMLParser::parseRank(xmlNode* node) {
    auto rank = std::make_shared<Rank>();
    rank->id = std::atoi((const char*)xmlGetProp(node, (const xmlChar*)"id"));
    rank->i_chunks = std::atoi((const char*)xmlGetProp(node, (const xmlChar*)"i_chunks"));
    rank->o_chunks = std::atoi((const char*)xmlGetProp(node, (const xmlChar*)"o_chunks"));
    rank->s_chunks = std::atoi((const char*)xmlGetProp(node, (const xmlChar*)"s_chunks"));
    ranks.push_back(rank);

    for (xmlNode* tb_node = node->children; tb_node; tb_node = tb_node->next) {
        if (tb_node->type == XML_ELEMENT_NODE && strcmp((const char*)tb_node->name, "tb") == 0) {
            parseThreadBlock(tb_node, rank);
        }
    }
}

void XMLParser::parseThreadBlock(xmlNode* node, std::shared_ptr<Rank>& rank) {
    auto tb = std::make_shared<ThreadBlock>();
    tb->id = std::atoi((const char*)xmlGetProp(node, (const xmlChar*)"id"));
    tb->send = std::atoi((const char*)xmlGetProp(node, (const xmlChar*)"send"));
    tb->recv = std::atoi((const char*)xmlGetProp(node, (const xmlChar*)"recv"));
    tb->chan = std::atoi((const char*)xmlGetProp(node, (const xmlChar*)"chan"));
    rank->tbs.push_back(tb);

    for (xmlNode* step_node = node->children; step_node; step_node = step_node->next) {
        if (step_node->type == XML_ELEMENT_NODE && strcmp((const char*)step_node->name, "step") == 0) {
            parseStep(step_node, tb);
        }
    }
}

void XMLParser::parseStep(xmlNode* node, std::shared_ptr<ThreadBlock>& tb) {
    auto step = std::make_shared<Step>();
    step->s = std::atoi((const char*)xmlGetProp(node, (const xmlChar*)"s"));
    step->type = op_string_to_enmu((const char*)xmlGetProp(node, (const xmlChar*)"type"));
    step->srcbuf = buffer_string_to_enmu((const char*)xmlGetProp(node, (const xmlChar*)"srcbuf"));
    step->srcoff = std::atoi((const char*)xmlGetProp(node, (const xmlChar*)"srcoff"));
    step->dstbuf = buffer_string_to_enmu((const char*)xmlGetProp(node, (const xmlChar*)"dstbuf"));
    step->dstoff = std::atoi((const char*)xmlGetProp(node, (const xmlChar*)"dstoff"));
    step->cnt = std::atoi((const char*)xmlGetProp(node, (const xmlChar*)"cnt"));
    step->depid = std::atoi((const char*)xmlGetProp(node, (const xmlChar*)"depid"));
    step->deps = std::atoi((const char*)xmlGetProp(node, (const xmlChar*)"deps"));
    step->hasdep = std::atoi((const char*)xmlGetProp(node, (const xmlChar*)"hasdep"));
    tb->steps.push_back(step);
}

void XMLParser::displayRanks() {
    for (const auto& rank : ranks) {
        std::cout << "Rank ID: " << rank->id << "\n";
        // Display more details as needed
        const auto& step = rank->tbs[0]->steps[1];
        std::cout << "Step: " << step->s << " " << step->type << " " << step->srcbuf << " " 
        << step->srcoff << " " << step->dstbuf << " " << step->dstoff << " " << step->cnt << " " 
        << step->depid << " " << step->deps << " " << step->hasdep << std::endl;
    }

}
