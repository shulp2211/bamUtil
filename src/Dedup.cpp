/*
 *  Copyright (C) 2010-2012  Regents of the University of Michigan
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <getopt.h>
#include "SamFile.h"
#include "Dedup.h"
#include "Logger.h"

const uint32_t Dedup::CLIP_OFFSET = 1000L;
const uint64_t Dedup::UNMAPPED_SINGLE_KEY = 0xffffffffffffffffULL;
const uint64_t Dedup::UNMAPPED_PAIRED_KEY = 0xfffffffffffffffeULL;
const uint32_t Dedup::EMPTY_RECORD_COUNT =  0xffffffffUL;
const int Dedup::PAIRED_QUALITY_OFFSET = 10000;
const int Dedup::MAX_REF_ID = 0xffff;
const int Dedup::LOOK_BACK = 1000;

void Dedup::dedupDescription()
{
    std::cerr << " dedup - Mark Duplicates\n";
}


void Dedup::description()
{
    dedupDescription();
}


void Dedup::usage() {
    std::cerr << "Usage: dedup (options) --in=<InputBamFile> --out=<OutputBamFile>\n" << std::endl;
    std::cerr << "Required parameters :" << std::endl;
    std::cerr << "-i/--in [infile]  : input BAM file name (must be sorted)" << std::endl;
    std::cerr << "-o/--out [outfile] : output BAM file name (same order with original file)" << std::endl;
    std::cerr << "Optional parameters : (see SAM format specification for details)" << std::endl;
    std::cerr << "-l/--log [logfile] : log and summary statistics (default: [outfile].log)" << std::endl;
    std::cerr << "-r/--rm  : Remove duplicates (default is to mark duplicates)" << std::endl;
    std::cerr << "-f/--force-unmark  : Allow mark-duplicated BAM file and force unmarking the duplicates" << std::endl;
    std::cerr << "                     Default is to throw errors when trying to run a mark-duplicated BAM" << std::endl;
    std::cerr << "-v/--verbose  : Turn on verbose mode" << std::endl;
    std::cerr << "\n" << std::endl;
}

int Dedup::execute(int argc, char** argv) 
{
    // Shift arguments due to format being ./bam dedup and then the args.
    ++argv;
    --argc;

    /* --------------------------------
     * process the arguments
     * -------------------------------*/
    std::string inFile, outFile, logFile;
    bool removeFlag = false;
    bool verboseFlag = false;
    bool forceFlag = false;
    char c;
    int optionIndex = 0;
    
    static struct option getoptLongOptions[] = 
        {
            { "in", required_argument, NULL, 'i' },
            { "out", required_argument, NULL, 'o' },
            { "rm", no_argument, NULL, 'r' },
            { "log", no_argument, NULL, 'l' },
            { "force-unmark", no_argument, NULL, 'c' },
            { "verbose", no_argument, NULL, 'v' },
            { NULL, 0, NULL, 0 },
        };
  
    while ( ( c = getopt_long(argc, argv, "i:o:rvl:csf", getoptLongOptions, &optionIndex) ) != -1 ) {
        switch(c) {
            case 'i':
                inFile = optarg;
                break;
            case 'o':
                outFile = optarg;
                break;
            case 'r':
                removeFlag = true;
                break;
            case 'v':
                verboseFlag = true;
                break;
            case 'l':
                logFile = optarg;
                break;
            case 'f':
                forceFlag = true;
                break;
            default:
                usage();
                std::cerr << "Illegal switch: " << c << std::endl;
                abort();
        }
    }

    if (inFile.empty()) {
        usage();
        std::cerr << "Specify an input file" << std::endl;
        abort();
    }

    if (outFile.empty()) {
        usage();
        std::cerr << "Specify an output file" << std::endl;
        abort();
    }

    if (logFile.empty() ) {
        logFile = outFile + ".log";
    }
    Logger::gLogger = new Logger(logFile.c_str(), verboseFlag);

    /* -------------------------------------------------------------------
     * The arguments are processed.  Prepare the input BAM file,
     * instantiate dedup, and construct the read group library map
     * ------------------------------------------------------------------*/

    SamFile samIn;

    samIn.OpenForRead(inFile.c_str());
    samIn.setSortedValidation(SamFile::COORDINATE);

    SamFileHeader header;
    samIn.ReadHeader(header);

    Dedup dedup;
    dedup.buildReadGroupLibraryMap(header);

    int lastReferenceID = -1;
    int lastPosition = -1;
    dedup.lastReference = -1;
    dedup.lastCoordinate = -1;

    // for keeping some basic statistics
    uint32_t recordCount = 0;
    uint32_t pairedCount = 0;
    uint32_t properPairCount = 0;
    uint32_t unmappedCount = 0;
    uint32_t reverseCount = 0;
    uint32_t qualCheckFailCount = 0;

    // Now we start reading records
    SamRecord record; 
    while(samIn.ReadRecord(header, record)) {

        // Take note of properties of this record
        int flag = record.getFlag();
        if ( flag & 0x0001 ) ++pairedCount;
        if ( flag & 0x0002 ) ++properPairCount;
        if ( flag & 0x0004 ) ++unmappedCount;
        if ( flag & 0x0010 ) ++reverseCount;
        if ( flag & 0x0200 ) ++qualCheckFailCount;
        if ( ( ( flag & 0x0400 ) > 0 ) && ( ! forceFlag ) ) {
            Logger::gLogger->error("There are records already duplicate marked.");
            Logger::gLogger->error("Use -f to clear the duplicate flag and start the deduping procedure over");
        }

        // put the record in the appropriate maps:
        //   single reads go in fragmentMap
        //   paired reads go in pairedMap
        recordCount = samIn.GetCurrentRecordCount();
        dedup.placeRecordInMaps(record, recordCount);

        // Let us know if we're moving to a new chromosome
        if (lastReferenceID != record.getReferenceID()) {
            lastReferenceID = record.getReferenceID();
            Logger::gLogger->writeLog("Reading ReferenceID %d\n", lastReferenceID);
        } else {
            int position = record.get0BasedPosition();
            // We're assuming the BAM file is sorted.  Otherwise, there is trouble.
            if ( ( position >= 0 ) && (position < lastPosition ) ) {
                Logger::gLogger->error("The BAM file is not sorted in the readName %s, reference sequence %s, position %d", 
                                       record.getReadName(), 
                                       header.getReferenceLabel(record.getReferenceID()).c_str(), 
                                       position+1);
            }
            lastPosition = position;
        }

        // if we have moved to a new position, look back at previous reads for duplicates
        if (dedup.hasPositionChanged(record)) {
            dedup.markDuplicatesBefore(record);
        }

        // let the user know we're not napping
        if (verboseFlag && (recordCount % 100000 == 0)) {
            Logger::gLogger->writeLog("recordCount=%u singleKeyMap=%u pairedKeyMap=%u, dictSize=%u", 
                                      recordCount, dedup.fragmentMap.size(), 
                                      dedup.pairedMap.size(), 
                                      dedup.readDataMap.size());
        }
    }

    // we're finished reading record so clean up the duplicate search and close the input file
    dedup.markDuplicatesBefore(Dedup::MAX_REF_ID, 0);
    samIn.Close();

    // print some statistics
    uint32_t totalDuplicates = dedup.singleDuplicates + 2*dedup.pairedDuplicates;
    Logger::gLogger->writeLog("--------------------------------------------------------------------------");
    Logger::gLogger->writeLog("SUMMARY STATISTICS OF THE READS");
    Logger::gLogger->writeLog("Total number of reads: %u",recordCount);
    Logger::gLogger->writeLog("Total number of paired-end reads: %u",pairedCount);
    Logger::gLogger->writeLog("Total number of properly paired reads: %u",properPairCount);
    Logger::gLogger->writeLog("Total number of unmapped reads : %u",unmappedCount);
    Logger::gLogger->writeLog("Total number of reverse strand mapped reads: %u",reverseCount);
    Logger::gLogger->writeLog("Total number of QC-failed reads : %u",qualCheckFailCount);
    Logger::gLogger->writeLog("Size of singleKeyMap (must be zero) : %u",dedup.fragmentMap.size());
    Logger::gLogger->writeLog("Size of pairedKeyMap (must be zero) : %u",dedup.pairedMap.size());
    Logger::gLogger->writeLog("Total number of duplicate single-ended or mate-unpaired reads : %u",dedup.singleDuplicates);
    Logger::gLogger->writeLog("Total number of duplicate paired-end reads (both ends mapped) : %u",dedup.pairedDuplicates);
    Logger::gLogger->writeLog("Total number of duplicated records: %u",totalDuplicates);
    Logger::gLogger->writeLog("--------------------------------------------------------------------------");
    Logger::gLogger->writeLog("Sorting the indices of %d duplicated records",dedup.duplicateIndices.size());

    // sort the indices of duplicate records
    std::sort(dedup.duplicateIndices.begin(), dedup.duplicateIndices.end(), std::less<uint32_t> ());

    // get ready to write the output file by making a second pass through the input file
    samIn.OpenForRead(inFile.c_str());
    samIn.ReadHeader(header);

    SamFile samOut;
    samOut.OpenForWrite(outFile.c_str());
    samOut.WriteHeader(header);

    // an iterator to run through the duplicate indices
    std::vector<uint32_t>::iterator currentDupIndex = dedup.duplicateIndices.begin();

    // let the user know what we're doing
    Logger::gLogger->writeLog("\nWriting %s", outFile.c_str());

    // count the duplicate records as a check
    uint32_t singleDuplicates(0), pairedDuplicates(0);

    // start reading records and writing them out
    while(samIn.ReadRecord(header, record)) {
        uint32_t currentIndex = samIn.GetCurrentRecordCount();

        bool foundDup = (currentIndex == *currentDupIndex);

        // modify the duplicate flag and write out the record, if it's appropriate
        int flag = record.getFlag();
        if (foundDup) {                     // this record is a duplicate
            record.setFlag( flag | 0x400 ); // mark duplicate
            currentDupIndex++;
            // increment duplicate counters to verify we found them all
            if ( ( ( flag & 0x0001 ) == 0 ) || ( flag & 0x0008 ) ) { // unpaired or mate unmapped
                singleDuplicates++;
            }
            else {
                pairedDuplicates++;
            }
            // write the record if we are not removing duplicates
            if (!removeFlag ) samOut.WriteRecord(header, record);
        }
        else if (forceFlag ) { 
            // this is not a duplicate we've identified but we want to remove any duplicate marking
            record.setFlag( flag & 0xfffffbff ); // unmark duplicate
            samOut.WriteRecord(header, record);
        }
        else { // not a duplicate we've identified and we aren't worried about existing marking
            samOut.WriteRecord(header, record);
        }
	
        // Let the user know we're still here
        if (verboseFlag && (currentIndex % 100000 == 0)) {
            Logger::gLogger->writeLog("recordCount=%u", currentIndex);
        }
    }

    // We're done.  Close the files and print triumphant messages.
    samIn.Close();
    samOut.Close();

    Logger::gLogger->writeLog("Successfully %s %u unpaired and %u paired duplicate reads", 
                              removeFlag ? "removed" : "marked" ,
                              singleDuplicates,
                              pairedDuplicates/2);
    Logger::gLogger->writeLog("\nDedup complete!");
    return 0;
}

// Now that we've reached coordinate on chromosome reference, look back to find any duplicates
void Dedup::markDuplicatesBefore(uint32_t reference, uint32_t coordinate) {
    // Find the key corresponding to the current position
    // We will first search through single reads up to this position
    uint64_t key = makeKey(reference, coordinate, false, 0);
    SingleKeyToReadDataPointerMapIterator fragmentFinish = fragmentMap.lower_bound(key);

    // For each key k < fragmentFinish, look through reads indexed by that key to find the
    //   one with the highest quality, mark the others as duplicates, and clean up the maps
    for (SingleKeyToReadDataPointerMapIterator iterator = fragmentMap.begin(); iterator != fragmentFinish; iterator++) {
        if (iterator->second.size() > 1) {
            int maxQuality = -1;
            ReadDataPointerVectorIterator maxRead = iterator->second.end();
            for (ReadDataPointerVectorIterator currentRead = iterator->second.begin(); 
                 currentRead != iterator->second.end(); 
                 currentRead++) {
                if ( (*currentRead)->getPairedBaseQuality() > maxQuality) {
                    maxQuality = (*currentRead)->getPairedBaseQuality();
                    maxRead = currentRead;
                }
            }
            if (maxRead == iterator->second.end()) {
                Logger::gLogger->error("In markDuplicates:  no positive best quality found.");
            }
            for (ReadDataPointerVectorIterator currentRead = iterator->second.begin(); 
                 currentRead != iterator->second.end(); 
                 currentRead++) {
                if (currentRead != maxRead) {
                    if ( (*currentRead)->paired == false) {
                        duplicateIndices.push_back((*currentRead)->recordCount1);
                        singleDuplicates++;
                    }
                }

                StringToReadDataPointerMapIterator readDataMapIterator = readDataMap.find( (*currentRead)->readName);
                if (readDataMapIterator == readDataMap.end()) {
                    Logger::gLogger->error("In markDuplicatesBefore:  Cannot find %s in readDataMap", ((*currentRead)->readName).c_str());
                }
                if (readDataMapIterator->second->paired == false) {
                    delete readDataMapIterator->second; 
                    readDataMap.erase(readDataMapIterator);
                }
            }	    
        } else {  // in this case, there are no duplicates
            StringToReadDataPointerMapIterator readDataMapIterator = readDataMap.find(iterator->second.front()->readName);
            if (readDataMapIterator->second->paired == false) {
                delete readDataMapIterator->second; 
                readDataMap.erase(readDataMapIterator);
            }
        }

    }
    if (fragmentFinish != fragmentMap.begin()) {
        fragmentMap.erase(fragmentMap.begin(), fragmentFinish);
    }

    // Now do the same thing with the paired reads
    PairedKey pairedKey(0, key);
    PairedKeyToReadDataPointerMapIterator pairedFinish = pairedMap.lower_bound(pairedKey);
    for (PairedKeyToReadDataPointerMapIterator iterator = pairedMap.begin(); iterator != pairedFinish; iterator++) {
        if (iterator->second.size() > 1) {
            int maxQuality = -1;
            ReadDataPointerVectorIterator maxRead = iterator->second.end();	
            for (ReadDataPointerVectorIterator currentRead = iterator->second.begin(); 
                 currentRead != iterator->second.end(); 
                 currentRead++) {
                if ( (*currentRead)->getPairedBaseQuality() > maxQuality) {
                    maxQuality = (*currentRead)->getPairedBaseQuality();
                    maxRead = currentRead;
                }
            }
            if (maxRead == iterator->second.end()) {
                Logger::gLogger->error("In markDuplicates:  no positive best quality found.");
            }

            for (ReadDataPointerVectorIterator currentRead = iterator->second.begin(); 
                 currentRead != iterator->second.end(); 
                 currentRead++) {
                if (currentRead != maxRead) {
                    duplicateIndices.push_back((*currentRead)->recordCount1);
                    duplicateIndices.push_back((*currentRead)->recordCount2);
                    pairedDuplicates++;
                }

                StringToReadDataPointerMapIterator readDataMapIterator = readDataMap.find( (*currentRead)->readName);
                if (readDataMapIterator == readDataMap.end()) {
                    Logger::gLogger->error("In markDuplicatesBefore:  Cannot find %s in readDataMap", ((*currentRead)->readName).c_str());
                }
                delete readDataMapIterator->second;  
                readDataMap.erase(readDataMapIterator);
            }
        } else { // there are no duplicates
            StringToReadDataPointerMapIterator readDataMapIterator = readDataMap.find(iterator->second.front()->readName);
            delete readDataMapIterator->second; 
            readDataMap.erase(readDataMapIterator);
        }
    }
    if (pairedFinish != pairedMap.begin()) {
        pairedMap.erase(pairedMap.begin(), pairedFinish);
    }
    return;
}

// Look at reads before this record and determine duplicates
void Dedup::markDuplicatesBefore(SamRecord& record) {
    uint32_t coordinate = record.get0BasedPosition();
    uint32_t lookBackCoordinate = coordinate - LOOK_BACK;
    if (lookBackCoordinate < 0) {
        lookBackCoordinate = 0;
    }
    return markDuplicatesBefore(record.getReferenceID(), lookBackCoordinate);
}

// determine whether the position of record is different from the previous record
bool Dedup::hasPositionChanged(SamRecord& record) {
    if (lastReference < record.getReferenceID() || lastCoordinate < record.get0BasedPosition()) {
        lastReference = record.getReferenceID();
        lastCoordinate = record.get0BasedPosition();
        return true;
    }
    return false;
}

// when record is read, we will put it into the appropriate maps
void Dedup::placeRecordInMaps(SamRecord& record, uint32_t recordCount) {
    int flag = record.getFlag();

    if (flag & 0x0002) properPair++;
    if (flag & 0x0004) {  // fragment is unmapped so ignore it
        unmapped++;
        return; 
    }

    // save some basic information about the record in a ReadData structure
    uint64_t key = makeKeyFromRecord(record);
    ReadData* readDataPointer = new ReadData();
    readDataPointer -> key1 = key;
    readDataPointer -> key2 = UNMAPPED_SINGLE_KEY;
    readDataPointer -> recordCount1 = recordCount;
    readDataPointer -> recordCount2 = EMPTY_RECORD_COUNT;
    readDataPointer -> readName = record.getReadName();
    readDataPointer -> baseQuality = getBaseQuality(record);

    if ( ( (flag & 0x0001) == 0) || (flag & 0x0008) ) {  
        // if it's a single read or its mate is unmapped, 
        //   label it as not paired and put it into fragmentMap
        if ((flag & 0x0001) == 0) singleRead++;
        readDataPointer -> paired = false;
        if(readDataMap.insert( std::pair <std::string, ReadData*> (record.getReadName(), readDataPointer)).second == false) {
            Logger::gLogger -> error("The fragment %s has already been read.", record.getReadName());
            return;
        }
        fragmentMap[key].push_back(readDataPointer);
        return;
    }

    // We have a paired end read with its mate mapped
    readDataPointer -> paired = true;

    // Let's determine if we have already seen the mate
    StringToReadDataPointerMapIterator earlierRead = readDataMap.find(record.getReadName());

    if(earlierRead == readDataMap.end()) { // We haven't seen this read before
        firstPair++;
        // put this in fragmentMap, record that we've seen it, and move on to the next record
        fragmentMap[key].push_back(readDataPointer);
        readDataMap.insert( std::pair <std::string, ReadData*> (record.getReadName(), readDataPointer));
        return;
    }

    // We have already seen this read's mate
    foundPair++;
    ReadData* readData = earlierRead->second;

    // Update the information in the ReadData structure
    fragmentMap[key].push_back(readDataPointer);
    readData -> baseQuality += getBaseQuality(record);

    if (key < readData -> key1) {
        readData -> key2 = readData -> key1;
        readData -> key1 = key;
        readData -> recordCount2 = readData -> recordCount1;
        readData -> recordCount1 = recordCount;
    } else {
        readData -> key2 = key;
        readData -> recordCount2 = recordCount;
    }

    // Put the ReadData structure in the paired key map and go to the next record
    PairedKey pkey(readData -> key1, readData -> key2);
    pairedMap[pkey].push_back(readData);

}


// Finds the total base quality of a read 
int Dedup::getBaseQuality(SamRecord & record) {
    const char* baseQualities = record.getQuality();
    int readLength = record.getReadLength();
    int quality = 0.;
    for(int i=0; i < readLength; ++i) {
        int q = static_cast<int>(baseQualities[i])-33;
        if ( q >= 15 ) quality += q;
    }
    return quality;
}

// makes a key from the chromosome number, coordinate, orientation, and libraryID
//   single reads with equal keys are duplicates
uint64_t Dedup::makeKey(uint32_t referenceID, uint32_t coordinate, bool orientation, uint32_t libraryID) {
    return ( (0xffff000000000000 & ( static_cast<uint64_t>(referenceID) << 48))  |
             (0x0000ffffffff0000 & ( static_cast<uint64_t>(coordinate + CLIP_OFFSET) << 16)) |
             (0x000000000000ff00 & ( static_cast<uint64_t>(orientation) << 8)) |
             (0x00000000000000ff & ( static_cast<uint64_t>(libraryID))));
}

// extract the relevant information from the key and make the record
uint64_t Dedup::makeKeyFromRecord(SamRecord& record) {
    int32_t referenceID = record.getReferenceID();
    bool orientation = (record.getFlag() & 0x0010) > 0;
    int32_t coordinate = orientation ? record.get0BasedUnclippedEnd() : record.get0BasedUnclippedStart();
    if ( ( referenceID < 0 ) || ( coordinate + CLIP_OFFSET < 0 ) ) {
        Logger::gLogger->error("Dedup::makeKeyFromRecord(record) - refID or coordinate is negative. refID = %d, coordinate = %d",
                               referenceID, coordinate + CLIP_OFFSET);
        return UNMAPPED_SINGLE_KEY; // this represents an unmapped read end
    }
    return makeKey(referenceID, coordinate, orientation, getLibraryID(record));
}

// build the read group library map
void Dedup::buildReadGroupLibraryMap(SamFileHeader& header) {
    rgidLibMap.clear();
    numLibraries = 0;
    std::map<std::string,uint32_t> libNameMap;
    
    SamHeaderRecord * headerRecord = header.getNextRGRecord();
    while(headerRecord != NULL) {
        std::string ID = headerRecord->getTagValue("ID");
        std::string LB = headerRecord->getTagValue("LB");

        if ( ID.empty() ) {
            std::string headerRecordString;
            headerRecord->appendString(headerRecordString);
            Logger::gLogger->error("Cannot find readGroup ID information in the header line %s", 
                                   headerRecordString.c_str());
        }
        if ( rgidLibMap.find(ID) != rgidLibMap.end() ) {
            Logger::gLogger->error("The readGroup ID %s is not a unique identifier",ID.c_str());
        }

        if ( LB.empty() ) {
            std::string headerRecordString;
            headerRecord->appendString(headerRecordString);
            Logger::gLogger->warning("Cannot find library information in the header line %s. Using empty string for library name",
                                     headerRecordString.c_str());
        }

        if ( libNameMap.find( LB ) != libNameMap.end() ) {
            rgidLibMap[ID] = libNameMap[LB];
        }
        else {
            numLibraries = libNameMap.size()+1;
            libNameMap[LB] = numLibraries;
            rgidLibMap[ID] = numLibraries;
        }
        headerRecord = header.getNextRGRecord();
    }

    if (numLibraries > 0xff) {
        Logger::gLogger->error("More than 255 library names are identified. Dedup currently only allows up to 255 library names");
    }
}    

// get the libraryID of a record
uint32_t Dedup::getLibraryID(SamRecord& record, bool checkTags) {
    if ( ( checkTags == false ) && ( numLibraries <= 1 ) ) {
        return 0; 
    } else {
        char tag[3];
        char vtype;
        void* value;
        std::string rgID;
        record.resetTagIter();
        while( record.getNextSamTag(tag,vtype,&value) != false ) {
            if ( ( tag[0] == 'R' ) && ( tag[1] == 'G' ) && ( vtype == 'Z' ) ) {
                if ( !rgID.empty() ) {
                    Logger::gLogger->error("Multiple RG tag found in one record. ReadName is %s",record.getReadName());
                }
                else if ( record.isStringType(vtype) ) {
                    String s = (String)*(String*)value;
                    rgID = s.c_str();
                }
                else {
                    Logger::gLogger->error("vtype is not string (Z) for RG tag");
                }
            }
        }
        if ( rgID.empty() ) {
            Logger::gLogger->error("No RG tag is found in read %s",record.getReadName());
            return 0;
        }
        else {
            std::map<std::string,uint32_t>::iterator it = rgidLibMap.find(rgID);
            if ( it != rgidLibMap.end() ) {
                return it->second;
            }
            else {
                Logger::gLogger->warning("RG tag %s does not exist in the header",rgID.c_str());
                return 0; // cannot be reached
            }
        }
    }
}