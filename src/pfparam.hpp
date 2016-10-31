/*
 * smcsmc is short for particle filters for ancestral recombination graphs.
 * This is a free software for demographic inference from genome data with particle filters.
 *
 * Copyright (C) 2013, 2014 Sha (Joe) Zhu and Gerton Lunter
 *
 * This file is part of smcsmc.
 *
 * smcsmc is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "mersenne_twister.h"
#include "param.h"
#include "segdata.hpp"
#include "exception.hpp"

using namespace std;

#ifndef PFARGPfParam
#define PFARGPfParam


struct NotEnoughArg : public InvalidInput{
    NotEnoughArg( string str ):InvalidInput( str ){
        this->reason = "Not enough parameters when parsing option: ";
        throwMsg = this->reason + this->src;
    }
    ~NotEnoughArg() throw() {}
};


struct UnknowArg : public InvalidInput{
  UnknowArg( string str ):InvalidInput( str ){
    this->reason = "Unknow option: ";
    throwMsg = this->reason + this->src;
  }
  ~UnknowArg() throw() {}
};

struct FlagsConflict : public InvalidInput{
  FlagsConflict( string str1, string str2 ):InvalidInput( str1 ){
    this->reason = "Flag: ";
    throwMsg = this->reason + this->src + string(" conflict with flag ") + str2;
  }
  ~FlagsConflict() throw() {}
};


struct OutOfEpochRange : public InvalidInput{
  OutOfEpochRange( string str1, string str2 ):InvalidInput( str1 ){
    this->reason = "Problem: epochs specified in -xr/-xc options out of range: ";
    this->src      = "\033[1;31m" + str1 + string(" is greater than ") + str2 + "\033[0m";
    throwMsg = this->reason + src;
  }
  ~OutOfEpochRange() throw() {}
};


struct OutOfRange : public InvalidInput{
  OutOfRange( string str1, string str2 ):InvalidInput( str1 ){
    this->reason = "Flag \"";
    throwMsg = this->reason + this->src + string(" ") + str2 + string("\" out of range [0, 1].");
  }
  ~OutOfRange() throw() {}
};


struct WrongType : public InvalidInput{
  WrongType( string str ):InvalidInput( str ){
    this->reason = "Wrong type for parsing: ";
    throwMsg = this->reason + this->src;
  }
  ~WrongType() throw() {}
};


/*!
 * \brief smcsmc parameters
 */
class PfParam{
  #ifdef UNITTEST
  friend class TestPfParam;
  #endif
  friend class ParticleContainer;

  public:
    // Constructors and Destructors
    PfParam();
    ~PfParam();

    //
    // Methods
    //
    void parse(int argc, char *argv[]);
    void printHelp();
    bool help() const { return help_; }
    void printVersion(std::ostream *output);
    bool version() const { return version_; }

    int  log( );
    void appending_Ne_file( bool hist = false );
    void outFileHeader();
    void appendToOutFile( size_t EMstep,
                          int epoch,
                          double epochBegin,
                          double epochEnd,
                          string eventType,
                          int from_pop,
                          int to_pop,
                          double opportunity,
                          double count,
                          double weight);

    void append_resample_file( int position, double ESS ) const;

    // ------------------------------------------------------------------
    // PfParameters
    // ------------------------------------------------------------------
    size_t N;        /*!< \brief Number of particles */
    int    EM_steps;     /*!< \brief Number of EM iterations */
    double ESSthreshold; /*!< \brief Effective sample size, scaled by the number of particles = ESS * N , 0 < ESS < 1 */

    bool   useCap;
    double Ne_cap;

    // ------------------------------------------------------------------
    // Action
    // ------------------------------------------------------------------
    double lag;
    bool calibrate_lag;
    double lag_fraction;
    bool online_bool;

    bool heat_bool;

    Segment * Segfile;
    Param *SCRMparam;
    Model model;
    MersenneTwister *rg ;
    vector<int> record_event_in_epoch;

    static const int RECORD_RECOMB_EVENT = 1;
    static const int RECORD_COALMIGR_EVENT = 2;

    double default_loci_length;

    double ESS() const { return this-> ESS_;} // scaled between zero and one

    double top_t() const { return this->top_t_;}

    void increaseEMcounter() { this->EMcounter_++; }
    size_t EMcounter() const { return EMcounter_; }

  private:

    //
    // Methods
    //
    void init();
    void reInit();
    void insertMutationRateInScrmInput();
    void insertRecombRateAndSeqlenInScrmInput();
    void insertSampleSizeInScrmInput();
    void finalize_scrm_input ( );
    void finalize ( );
    void convert_scrm_input();
    void writeLog(ostream * writeTo);


    void nextArg(){
        string tmpFlag = *argv_i;
        ++argv_i;
        if (argv_i == argv_.end() || (*argv_i)[0] == '-' ) {
            throw NotEnoughArg (tmpFlag);
        }
    }


    template<class T>
    T convert(const std::string &arg) {
        T value;
        std::stringstream ss(arg);
        ss >> value;
        if (ss.fail()) {
        throw WrongType(arg);
        }
        return value;
    }


    template<class T>
    T readNextInput() {
        this->nextArg();
        return convert<T>(*argv_i);
    }


    int readRange(int& second) {
        this->nextArg();
        char c;
        double input;
        std::stringstream ss(*argv_i);
        ss >> input;
        second = input;
        if (ss.fail() || input < 1) throw std::invalid_argument(std::string("Failed to parse int (e.g. '1') or int range (e.g. '1-10'): ") + *argv_i);
        if (!ss.get(c)) return input;
        if (c != '-') throw std::invalid_argument(std::string("Parsing failure: expected '-' after int in range expression: ") + *argv_i);
        ss >> second;
        if (ss.fail() || second <= input || ss.get(c)) throw std::invalid_argument(std::string("Failed to parse int or int range: ") + *argv_i);
        return input;
    }

    // Members
    int argc_;
    std::vector<std::string> argv_;
    std::vector<std::string>::iterator argv_i;

    // ------------------------------------------------------------------
    // PfParameters
    // ------------------------------------------------------------------
    bool   ESS_default_bool;
    string scrm_input;
    bool   EM_bool;
    size_t EMcounter_;
    double original_recombination_rate_;

    // ------------------------------------------------------------------
    // Default values
    // ------------------------------------------------------------------
    size_t default_nsam;
    double default_mut_rate;
    double default_recomb_rate;
    double default_num_mut;

    // ------------------------------------------------------------------
    // Input
    // ------------------------------------------------------------------
    string pattern;     /*! population segement pattern */
    double top_t_;
    double ESS_;   // scaled between zero and one

    // Segdata related
    string input_SegmentDataFileName;

    // ------------------------------------------------------------------
    // Output Related
    // ------------------------------------------------------------------
    bool log_bool;
    string out_NAME_prefix;
    //string HIST_NAME;
    //string Ne_NAME;
    string outFileName;
    string log_NAME;
    string TMRCA_NAME;
    string WEIGHT_NAME;
    //string BL_NAME;
    //string SURVIVOR_NAME;
    string Resample_NAME;
    int heat_seq_window;

    // ------------------------------------------------------------------
    // Version Info
    // ------------------------------------------------------------------
    string smcsmcVersion;
    string scrmVersion;
    string compileTime;

    // Help related
    bool help_;
    void setHelp(const bool help) { this->help_ = help; }
    bool version_;
    void setVersion(const bool version) { this->version_ = version; }

    void helpOption();
    void helpExample();
};

#endif
