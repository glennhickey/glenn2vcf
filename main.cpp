// main.cpp: Main file for core graph merger

#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <set>
#include <utility>
#include <algorithm>
#include <getopt.h>

#include "ekg/vg/vg.hpp"
#include "ekg/vg/index.hpp"
#include "ekg/vg/vcflib/src/Variant.h"

/**
 * Represents our opinion of a particular base in a node in the graph.
 */
struct BaseCall {
    // How many alts are allowed?
    static const int MAX_ALTS = 2;

    // Is the default base here peresnt?
    bool graphBasePresent = false;
    // How many alts are here?
    char numberOfAlts = 0;
    // What are the actual alt bases?
    char alts[MAX_ALTS];
    
    /**
     * Create a new BaseCall representing what's going on at this position in
     * the graph. Uses the calls from the Glenn file (up to two one-character
     * strings). This constructor is responsible for interpreting the "-" and
     * "." special call characters.
     */
    BaseCall(const std::set<std::string>& altSet) {
        
        // We start with no alts.
        numberOfAlts = 0;
        // And with no indication that this base is present in the graph.
        graphBasePresent = false;
        for(auto alt : altSet) {
            if(alt == "-") {
                // This isn't a real alt base. It just means "same as the other
                // character". Skip it.
                continue;
            } else if(alt == ".") {
                // The occurrence of this character means that the graph's
                // normal base is actually present.
                graphBasePresent = true;
                continue;
            }
            // Otherwise we got a real letter.
            // Make sure we're not going out of bounds
            assert(alt.size() == 1);
            assert(numberOfAlts < MAX_ALTS);
            // Store each alt at the next position in  the array.
            alts[numberOfAlts] = alt[0];
            numberOfAlts++;
        }
    }
    
    /**
     * Default constructor for being put in a vector.
     */
    BaseCall() {
        // Nothing to do!
    }
};

/**
 * Make a letter into a full string because apparently that's too fancy for the
 * standard library.
 */
std::string char_to_string(const char& letter) {
    std::string toReturn;
    toReturn.push_back(letter);
    return toReturn;
}

/**
 * Write a minimal VCF header for a single-sample file.
 */
void write_vcf_header(std::ostream& stream, std::string sample_name) {
   stream << "##fileformat=VCFv4.2" << std::endl;
   stream << "##FORMAT=<ID=GT,Number=1,Type=Integer,Description=\"Genotype\">" << std::endl;
   stream << "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\t" << sample_name << std::endl;
}

/**
 * Create the reference allele for an empty vcflib Variant, since apaprently
 * there's no method for that already. Must be called before any alt alleles are
 * added.
 */
void create_ref_allele(vcflib::Variant& variant, const std::string& allele) {
    // Set the ref allele
    variant.ref = allele;
    // Make it 0 in the alleles-by-index list
    variant.alleles.push_back(allele);
    // Build the reciprocal index-by-allele mapping
    variant.updateAlleleIndexes();
}

/**
 * Add a new alt allele to a vcflib Variant, since apaprently there's no method
 * for that already.
 */
void add_alt_allele(vcflib::Variant& variant, const std::string& allele) {
    // Add it as an alt
    variant.alt.push_back(allele);
    // Make it next in the alleles-by-index list
    variant.alleles.push_back(allele);
    // Build the reciprocal index-by-allele mapping
    variant.updateAlleleIndexes();
}

/**
 * Return true if a mapping is a perfect match, and false if it isn't.
 */
bool mapping_is_perfect_match(const vg::Mapping& mapping) {
    for (auto edit : mapping.edit()) {
        if (edit.from_length() != edit.to_length() || !edit.sequence().empty()) {
            // This edit isn't a perfect match
            return false;
        }
    }
    
    // If we get here, all the edits are perfect matches.
    // Note that Mappings with no edits at all are full-length perfect matches.
    return true;
}

void help_main(char** argv) {
    std::cerr << "usage: " << argv[0] << " [options] VGFILE GLENNFILE" << std::endl
        << "Convert a Glenn-format vg graph and variant file pair to a VCF." << std::endl
        << std::endl
        << "There are three objects in play: the reference (a single path), "
        << "the graph (containing the reference as a path) and the sample "
        << "(which is a set of calls on the graph, with some substitutions, "
        << "defined by the Glenn file)."
        << std::endl
        << "options:" << std::endl
        << "    -r, --ref PATH      use the given path name as the reference path" << std::endl
        << "    -h, --help          print this help message" << std::endl;
}

int main(int argc, char** argv) {
    
    if(argc == 1) {
        // Print the help
        help_main(argv);
        return 1;
    }
    
    // Option variables
    // What's the name of the reference path in the graph?
    std::string refPathName = "ref";
    // What name should we use for the sample in the VCF file?
    std::string sampleName = "SAMPLE";
    
    optind = 1; // Start at first real argument
    bool optionsRemaining = true;
    while(optionsRemaining) {
        static struct option longOptions[] = {
            {"ref", required_argument, 0, 'r'},
            {"help", no_argument, 0, 'h'},
            {0, 0, 0, 0}
        };

        int optionIndex = 0;

        char option = getopt_long(argc, argv, "r:h", longOptions, &optionIndex);
        switch(option) {
        // Option value is in global optarg
        case 'r':
            // Set the reference path name
            refPathName = optarg;
        case -1:
            optionsRemaining = false;
            break;
        case 'h': // When the user asks for help
        case '?': // When we get options we can't parse
            help_main(argv);
            exit(1);
            break;
        default:
            std::cerr << "Illegal option: " << option << std::endl;
            exit(1);
        }
    }
    
    if(argc - optind < 2) {
        // We don't have two positional arguments
        // Print the help
        help_main(argv);
        return 1;
    }
    
    // Pull out the file names
    std::string vgFile = argv[optind++];
    std::string glennFile = argv[optind++];
    
    // Open the vg file
    std::ifstream vgStream(vgFile);
    if(!vgStream.good()) {
        std::cerr << "Could not read " << vgFile << std::endl;
        exit(1);
    }
    
    // Load up the VG file
    vg::VG vg(vgStream);
    
    // Make sure the reference path is present
    assert(vg.paths.has_path(refPathName));
    
    // Trace the reference path, and assign each node a canonical reference
    // range. The first base of the node occurs at the given position in the
    // reference. Some nodes may be backward (orientation true) at their
    // canonical reference positions. In this case, the last base of the node
    // occurs at the given position.
    std::map<int64_t, std::pair<size_t, bool>> referencePositionAndOrientation;
    
    // We're also going to build the reference sequence string
    std::stringstream refSeqStream;
    
    // We also need to be able to map any given position in the reference to the
    // NodeTraversal that lives there. We store a node under its lowest
    // reference base number, turn the sort of the map backwards, and use
    // lower_bound for the find (greatest element with key not greater than
    // query). See <http://stackoverflow.com/a/529880>
    std::map<size_t, vg::NodeTraversal, std::greater<size_t>> nodesByReference;
    
    // What base are we at in the reference
    size_t referenceBase = 0;
    for(auto mapping : vg.paths.get_path(refPathName)) {
        // All the mappings need to be perfect matches.
        assert(mapping_is_perfect_match(mapping));
    
        if(!referencePositionAndOrientation.count(mapping.position().node_id())) {
            // This is the first time we have visited this node in the reference
            // path.
            
            // Add in a mapping.
            referencePositionAndOrientation[mapping.position().node_id()] = 
                std::make_pair(referenceBase, mapping.is_reverse());
        }
        
        // Find the node's sequence
        auto& sequence = vg.get_node(mapping.position().node_id())->sequence();
        
        // Add it to our idea of the reference string
        refSeqStream << sequence;
        
        // Say that this node appears here along the reference in this
        // orientation.
        nodesByReference[referenceBase] = vg::NodeTraversal(
            vg.get_node(mapping.position().node_id()), mapping.is_reverse()); 
        
        // Whether we found the right place for this node in the reference or
        // not, we still need to advance along the reference path. We assume the
        // whole node is included in the path (since it sort of has to be,
        // syntactically, unless it's the first or last node).
        referenceBase += sequence.size();
    }
    
    // Create the actual reference sequence we will use
    std::string refSeq(refSeqStream.str());
    
    // Announce progress.
    std::cerr << "Traced " << referenceBase << " bp reference path " << refPathName << "." << std::endl;
    
    if(refSeq.size() < 100) {
        std::cerr << "Reference sequence: " << refSeq << std::endl;
    }
    
    // Open up the Glenn-file
    std::ifstream glennStream(glennFile);
    
    // Parse it into an internal format, where we keep track of whether bases
    // exist or not.
    // Stores call info for a position by graph node and index in the node.    
    std::map<int64_t, std::vector<BaseCall>> callsByNodeOffset;
    
    // Loop through all the lines
    std::string line;
    while(std::getline(glennStream, line)) {
        // For each line
        
        if(line == "") {
            // Skip blank lines
            continue;
        }
        
        // Make a stringstream to read out tokens
        std::stringstream tokens(line);
        
        // Read the node id
        int64_t nodeId;
        tokens >> nodeId;
        
        // Read the offset
        size_t offset;
        tokens >> offset;
        offset--; // Make it 0-based
        
        // Read the base that the graph has at this position
        char graphBase;
        tokens >> graphBase;
        
        // Read the call string
        std::string call;
        tokens >> call;
        
        // Split that out into a set of call character strings. This is how we
        // split on commas in C++. Ask for the non-matched parts of a regex
        // iterator that matches commas.
        std::set<std::string> callCharacters(std::sregex_token_iterator(
            call.begin(), call.end(), std::regex(","), -1),
            std::sregex_token_iterator());
        
        // Fill in the callsByNodeOffset map for this base of this node.
        
        if(callsByNodeOffset[nodeId].size() <= offset) {
            // Make sure there's room in the vector
            callsByNodeOffset[nodeId].resize(offset + 1);
        }
        
        // Interpret the meaning of the -,. or A,C type character pairs that
        // Glenn is using.
        callsByNodeOffset[nodeId][offset] = BaseCall(callCharacters);
        
        std::cerr << "Node " << nodeId << " base " << offset << " status: "
            << (callsByNodeOffset[nodeId][offset].graphBasePresent ? "Present" : "Absent")
            << std::endl;
    }
    
    // Generate a vcf header. We can't make Variant records without a
    // VariantCallFile, because the variants need to know which of their
    // available info fields or whatever are defined in the file's header, so
    // they know what to output.
    std::stringstream headerStream;
    // TODO: get sample name from file or a command line option.
    write_vcf_header(headerStream, sampleName);
    
    // Load the headers into a new VCF file object
    vcflib::VariantCallFile vcf;
    std::string headerString = headerStream.str();
    assert(vcf.openForOutput(headerString));
    
    // Spit out the header
    std::cout << headerStream.str();
    
    // Then go through it from the graph's point of view: first over alt nodes
    // backending into the reference (creating things occupying ranges to which
    // we can attribute copy number) and then over reference nodes.
    
    vg.for_each_node([&](vg::Node* node) {
        // Look at every node in the graph and spit out variants for the ones
        // that are non-reference, but attach to two reference nodes and are
        // called as present.
    
        // Ensure this node is nonreference
        if(referencePositionAndOrientation.count(node->id())) {
            // Skip reference nodes
            return;
        }
        
        // Ensure this node attaches to two reference nodes, with correct
        // orientations.
        
        // Get all the oriented nodes to the left of this one's local forward.
        vector<vg::NodeTraversal> prevNodes;
        vg.nodes_prev(vg::NodeTraversal(node), prevNodes);
        
        // Find the leftmost reference node we're attached to at our start.
        size_t leftmostInPosition = (size_t) -1;
        vg::NodeTraversal leftmostInNode;
        
        for(auto& candidate : prevNodes) {
            // Look it up in the reference
            auto found = referencePositionAndOrientation.find(candidate.node->id());
            if(found == referencePositionAndOrientation.end()) {
                // We only care about nodes that actually are in the reference
                continue;
            }
            
            if((*found).second.first < leftmostInPosition) {
                // Take this as our new leftmost way into this node from the
                // reference. We know the reference positions are strictly
                // ordered, so orientation in the reference doesn't matter here.
                leftmostInNode = candidate;
                leftmostInPosition = (*found).second.first;
            }
        }
        
        // Get all the oriented nodes to the right of this one's local forward.
        vector<vg::NodeTraversal> nextNodes;
        vg.nodes_next(vg::NodeTraversal(node), nextNodes);
        
        // Find the leftmost reference node we're attached to at our end.
        size_t leftmostOutPosition = (size_t) -1;
        vg::NodeTraversal leftmostOutNode;
        
        for(auto& candidate : nextNodes) {
            // Look it up in the reference
            auto found = referencePositionAndOrientation.find(candidate.node->id());
            if(found == referencePositionAndOrientation.end()) {
                // We only care about nodes that actually are in the reference
                continue;
            }
            
            if((*found).second.first < leftmostOutPosition) {
                // Take this as our new leftmost way into this node from the
                // reference. We know the reference positions are strictly
                // ordered, so orientation in the reference doesn't matter here.
                leftmostOutNode = candidate;
                leftmostOutPosition = (*found).second.first;
            }
        }
        
        // Now check the above to make sure we're actually placed in a
        // consistent place in the reference. We need to be able to read along
        // the reference forward, into this node, and out the other end into the
        // reference later in the same orientation.
        if(leftmostInNode.node == nullptr || leftmostOutNode.node == nullptr) {
            // We're missing a reference node on one side.
            std::cerr << "Node " << node->id() << " not anchored to reference." << std::endl;
            return;
        }
        
        // Determine if we read into this node forward along the reference
        // (true) or backward along the reference (false). If we found the node
        // to our left in the same orientation as it occurs in the reference,
        // then we do read in forward.
        bool readInForward = leftmostInNode.backward == referencePositionAndOrientation.at(leftmostInNode.node->id()).second;
        
        // If we found the node to our right in the same orientation as it
        // occurs in the reference, then we do read out forward as well.
        bool readOutForward = leftmostOutNode.backward == referencePositionAndOrientation.at(leftmostOutNode.node->id()).second;
        
        if(readInForward != readOutForward) {
            // Going through this node would cause us to invert the direction
            // we're traversing the reference in.
            std::cerr << "Node " << node->id() << " inverts reference path." << std::endl;
            return;
        }
        
        // We need to work out what orientation we have relative to the
        // reference.
        vg::NodeTraversal altNode(node);
        
        if(!readInForward) {
            // We have a consistent orientation, but it's backward!
            // Swap the in and out nodes, and traverse our node in reverse.
            altNode.backward = true;
            std::swap(leftmostInNode, leftmostOutNode);
        }
        
        // Now we know that the in node really is where we come into the alt,
        // and the out node really is where we leave the alt, when reading along
        // the reference path. Either may still be backward in the reference
        // path, though.
        
        // Work out where and how they are positioned in the reference
        auto& inNodePlacement = referencePositionAndOrientation.at(leftmostInNode.node->id());
        auto& outNodePlacement = referencePositionAndOrientation.at(leftmostOutNode.node->id());
        
        if(outNodePlacement.first <= inNodePlacement.first) {
            // We're perfectly fine, orientation-wise, except we let you time
            // travel and leave before you arrived.
            std::cerr << "Node " << node->id() << " allows duplication." << std::endl;
            return;
        }
        
        // So what are the actual bounds of the reference interval covered by
        // the node? Since the node placement positions are just the first bases
        // along the reference at which the nodes occur, we don't care about
        // orientation of the anchoring node sequences.
        size_t referenceIntervalStart = inNodePlacement.first + leftmostInNode.node->sequence().size();
        size_t referenceIntervalPastEnd = outNodePlacement.first;
        assert(referenceIntervalPastEnd >= referenceIntervalStart);
        // How long is this interval in the reference?
        size_t referenceIntervalSize = referenceIntervalPastEnd - referenceIntervalStart;
        
        // Determine if this node is present throughout
        bool nodeFullyPresent = true;
        // And if any of it is present at all
        bool nodePartlyPresent = false;
        // Determine how many alt calls on the node are also present. TODO:
        // since we aren't going to list these as variants, should we just
        // ignore them?
        int maxAltsPresent = 0;
        for(auto& call : callsByNodeOffset[node->id()]) {
            // For every base in the node, note if it's graph base is present.
            nodeFullyPresent = nodeFullyPresent && call.graphBasePresent;
            nodePartlyPresent = nodePartlyPresent || call.graphBasePresent;
            
            // And note how many alts are also present.
            maxAltsPresent = std::max(maxAltsPresent, (int)call.numberOfAlts);
        }
        
        if(!nodePartlyPresent) {
            // This node isn't used at all in this sample, so ignore it.
            return;
        }
        
        if(nodePartlyPresent && !nodeFullyPresent) {
            // We shouldn't call this as a variant; they're not even
            // heterozygous this alt.
            std::cerr << "Node " << node->id() <<" is nonreference attached to "
                << "reference, but only partially present. Skipping!"
                << std::endl;
            return;
        }
        
        if(maxAltsPresent > 0) {
            // This node is present, but it has alts we should call on it and
            // won't.
            std::cerr << "Node " << node->id() <<" is nonreference attached to "
                << "reference, and present, but has additional novel alts!"
                << std::endl;
            // TODO: we leave the node in, because at least one copy of it
            // exists, but we might end up calling it homozygous when really we
            // have one of it and one of a modified version of it.
        }
        
        // Trace the reference between our in node and our out node.
        size_t refPosition = referenceIntervalStart;
        
        // We want to know if the reference path opposite us is ever called as
        // present or has a novel SNP. If so, since we're present, we know we
        // must be heterozygous here. If not, we'll call ourselves homozygous
        // here. TODO: catch conflicts between homozygous non-reference mutually
        // exclusive variants.
        // This is false by default; we assume it's missing and can be proven wrong.
        // TODO: this makes us call insertions as homozygous.
        bool refPathExists = false;
        
        while(refPosition < referenceIntervalPastEnd) {
            // While we aren't at the start of the reference node that comes
            // after this alt...
            
            // Get the node starting here in the reference. TODO: We don't
            // really need the lower bound and the fancy map; we can know the
            // reference start positions exactly.
            auto refNodeResult = nodesByReference.lower_bound(refPosition);
            
            // It must exist
            assert(refNodeResult != nodesByReference.end());
            
            // Grab the actual node
            vg::Node* refNode = (*refNodeResult).second.node;
            
            // We know we can iterate over the whole reference node, because it
            // must start immediatelty after the previous node ends.
            for(int i = 0; i < refNode->sequence().size(); i++) {
                // See if the reference node is ever called as absent with no
                // novel SNP alt.
                auto& call = callsByNodeOffset[refNode->id()][i];
                if(call.graphBasePresent || call.numberOfAlts > 0) {
                    // We found evidence the reference exists in alternation
                    // with this allele.
                    refPathExists = true;
                }
            }
            
            // Advance to the start of the next reference node
            refPosition += refNode->sequence().size();
        }
        
        // Make a Variant
        vcflib::Variant variant;
        variant.setVariantCallFile(vcf);
        variant.quality = 0;
        
        // Pull out the string for the reference allele
        std::string refAllele = refSeq.substr(referenceIntervalStart, referenceIntervalSize);
        // And for the alt allele
        std::string altAllele = altNode.node->sequence();
        if(altNode.backward) {
            // If the node is traversed backward, we need to flip its sequence.
            altAllele = vg::reverse_complement(altAllele);
        }
        
        if(refAllele.size() == 0) {
            // Shift everybody left by 1 base for the anchoring base that VCF
            // requires for insertions.
            assert(referenceIntervalStart != 0);
            referenceIntervalStart--;
            referenceIntervalSize++;
            // Add that base to the start of both alleles.
            refAllele = refSeq[referenceIntervalStart] + refAllele;
            altAllele = refSeq[referenceIntervalStart] + altAllele;
        }
        
        // Alt allele size can't be 0, no need to do the same shift for
        // deletions
        
        // Set the variant position. Convert to 1-based.
        variant.position = referenceIntervalStart + 1;
        
        // Initialize the ref allele
        create_ref_allele(variant, refAllele);
        
        // Add the graph version
        add_alt_allele(variant, altAllele);
        
        // Say we're going to spit out the genotype for this sample.        
        variant.format.push_back("GT");
        variant.outputSampleNames.push_back(sampleName);
        auto& genotype = variant.samples[sampleName]["GT"];
        
        // Make it hom/het as appropriate
        if(refPathExists) {
            // We're allele 1 (alt) and allele 2 (ref) heterozygous.
            genotype.push_back("1/0");
        } else {
            // We're allele 1 (alt) homozygous, other overlapping variants
            // notwithstanding.
            genotype.push_back("1/1");
        }

        std::cerr << "Found variant " << refAllele << " -> " << altAllele
            << " caused by node " << altNode.node->id()
            << " at 1-based reference position " << variant.position
            << std::endl;

        // Output the created VCF variant.
        std::cout << variant << std::endl;
        
    });
    
    vg.for_each_node([&](vg::Node* node) {
        // Now we go through all the nodes on the reference path, and add in
        // SNPs on them.
        
        // Ensure this node is nonreference
        if(!referencePositionAndOrientation.count(node->id())) {
            // Skip reference nodes
            return;
        }
        
        for(int i = 0; i < node->sequence().size(); i++) {
            // For each position along the node, grab the call there.
            auto& call = callsByNodeOffset[node->id()][i];
            if(call.numberOfAlts == 0) {
                // No variants here
                continue;
            }
            // At least one alt is present here.
            // Make the variant.
            
            // Make a Variant
            vcflib::Variant variant;
            variant.setVariantCallFile(vcf);
            variant.quality = 0;
            
            // Work out where it is in the reference
            size_t referencePosition = referencePositionAndOrientation.at(node->id()).first;
            if(referencePositionAndOrientation.at(node->id()).second) {
                // We're backward in the reference, so incrementing i goes
                // towards the start, and i max gives us our noted reference
                // position.
                referencePosition += (node->sequence().size() - i - 1);
            } else {
                // We're forward in the reference, so incrementing i goes
                // towards the end.
                referencePosition += i;
            }
            
            // Grab its reference base
            std::string refAllele = char_to_string(refSeq[referencePosition]);
            // Initialize the ref allele
            create_ref_allele(variant, refAllele);
            
            // Add in alt bases, with the right orientation
            for(int j = 0; j < call.numberOfAlts; j++) {
                std::string altAllele = char_to_string(call.alts[j]);
                if(referencePositionAndOrientation.at(node->id()).second) {
                    // We need to flip the orientation to reference
                    // orientation
                    altAllele = vg::reverse_complement(altAllele);
                }
                // Add the novel SNP allele
                add_alt_allele(variant, altAllele);
            }
            
            // Set the variant position. Convert to 1-based.
            variant.position = referencePosition + 1;
            
            // Say we're going to spit out the genotype for this sample.        
            variant.format.push_back("GT");
            variant.outputSampleNames.push_back(sampleName);
            auto& genotype = variant.samples[sampleName]["GT"];
            
            // Make it hom/het as appropriate
            if(call.graphBasePresent) {
                // We have the ref and, since we have a variant, we also have
                // the alt.
                genotype.push_back("1/0");
            } else if(call.numberOfAlts == 1) {
                // We have only one alt allele, and no reference. TODO: are we
                // really in alternation with a known alt path that took some of
                // our copy number?
                genotype.push_back("1/1");
            } else if(call.numberOfAlts == 2) {
                // We have two alt alleles and no reference. We must have both
                // present.
                genotype.push_back("1/2");
            } else {
                // This should never happen
                throw std::runtime_error("Semantically invalid BaseCall");
            }

            // TODO: determine if we're overlapping some other known alt that's
            // called as present, and call heterozygous alt/ref if we have no
            // ref present and just one alt.
            
            std::cerr << "Found variant " << refAllele << " -> ";
            for(int j = 0; j < call.numberOfAlts; j++) {
                std::cerr << call.alts[j] << ",";
            }
            std::cerr << " on node " << node->id()
                << " at 1-based reference position " << variant.position
                << std::endl;
                
                
            // Output the created VCF variant.
            std::cout << variant << std::endl;
        }
    });
    
    return 0;
}


