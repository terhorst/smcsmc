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

#include "particleContainer.hpp"

/*! \brief Particle filtering Initialization
 * Create particle initial states in the simulation
 *
 * \ingroup group_pf_init
 */
ParticleContainer::ParticleContainer(Model* model,
                                     MersenneTwister *rg,
                                     const vector<int>& record_event_in_epoch,
                                     size_t Num_of_states,
                                     double initial_position,
                                     bool heat_bool,
                                     bool emptyFile,
                                     vector <int> first_allelic_state) {
    this->heat_bool_ = heat_bool;
    this->random_generator_ = rg;
    this->set_ESS(0);
    this->set_current_printing_base(0);
    bool make_copies_of_model_and_rg = false; // Set to true to use a random generator, and model per particle for multithreading (slower!)
    dout << " --------------------   Particle Initial States   --------------------" << std::endl;
    for ( size_t i=0; i < Num_of_states ; i++ ){
        ForestState* new_state = new ForestState( model, rg, record_event_in_epoch, make_copies_of_model_and_rg );  // create a new state, using scrm; scrm always starts at 0.
        // Initialize members of FroestState (derived class members)
        new_state->init_EventContainers( model );
        new_state->buildInitialTree();
        new_state->setSiteWhereWeightWasUpdated( initial_position );
        if ( this->heat_bool_ ) {
            TmrcaState tmrca( 0, new_state->local_root()->height() );
            new_state->TmrcaHistory.push_back ( tmrca );
        }
        this->push(new_state, 1.0/Num_of_states );
        // If no data was given, the initial tree should not include any data
        if ( emptyFile ){
            new_state->include_haplotypes_at_tips(first_allelic_state);
        }
        #ifdef _SCRM
        cout << new_state->newick( new_state->local_root() ) << ";" << endl;
        #endif
    }
}


/*! \brief Resampling step
 *  If the effective sample size is less than the ESS threshold, do a resample, currently using systemetic resampling scheme.
 */
void ParticleContainer::ESS_resampling(valarray<double> weight_cum_sum, valarray<int> &sample_count, int mutation_at, double ESSthreshold, int num_state)
{
    dout << "At pos " << mutation_at << " ESS is " <<  this->ESS() <<", number of particle is " <<  num_state << ", and ESSthreshold is " << ESSthreshold <<endl;
    double ESS_diff = ESSthreshold - this->ESS();
    if ( ESS_diff > 1e-6 ) { // resample if the effective sample size is small, to check this step, turn the if statement off
        resampledout<<" ESS_diff = " << ESS_diff<<endl;
        resampledout << " ### PROGRESS: ESS resampling" << endl;
        this->systematic_resampling( weight_cum_sum, sample_count, num_state);
        this->resample(sample_count);
    }
}



/*! \brief Particle filtering Resampling step
 *
 *  Update particles according to the sample counts, and set the particle probabilities to 1
 *
 * \ingroup group_resample
 */
void ParticleContainer::resample(valarray<int> & sample_count){
    dout << endl;
    resampledout << " Recreate particles" << endl;
    resampledout << " ****************************** Start making list of new states ****************************** " << std::endl;
    resampledout << " will make total of " << sample_count.sum()<<" particle states" << endl;
    size_t number_of_particles = sample_count.size();
    for (size_t old_state_index = 0; old_state_index < number_of_particles; old_state_index++) {
        if ( sample_count[old_state_index] > 0 ) {
            ForestState * current_state = this->particles[old_state_index];
            // we need at least one copy of this particle; it keeps its own random generator
            resampledout << " Keeping  the " << std::setw(5) << old_state_index << "th particle" << endl;
            this->push(current_state); // The 'push' implementation sets the particle weight to 1
            // create new copy of the resampled particle
            for (int ii = 2; ii <= sample_count[old_state_index]; ii++) {
                resampledout << " Making a copy of the " << old_state_index << "th particle ... " ;
                dout << std::endl;
                dout << "current end position for particle current_state " << current_state->current_base() << endl;
                for (size_t ii =0 ; ii < current_state ->rec_bases_.size(); ii++){
                    dout << current_state ->rec_bases_[ii] << " ";
                }
                dout <<endl;

                ForestState* new_copy_state = new ForestState( *this->particles[old_state_index] );
                dout <<"making particle finished" << endl; // DEBUG

                // Resample the recombination position, and give particle its own event history
                if ( new_copy_state->current_base() < new_copy_state->next_base() ){ // Resample new recombination position if it has not hit the end of the sequence.
                    new_copy_state->resample_recombination_position();
                }
                // The 'push' implementation sets the particle weight to 1
                this->push(new_copy_state);
            }
        } else {
            resampledout << " Deleting the " << std::setw(5) << old_state_index << "th particle ... " ;
            delete this->particles[old_state_index];
        }

        this->particles[old_state_index]=NULL;
    }

    for (int i = 0; i < number_of_particles; i++) {
        this->particles[i] = this->particles[i + number_of_particles];
    }
    this -> particles.resize(number_of_particles);

    resampledout << "There are " << this->particles.size() << "particles in total." << endl;
    resampledout << " ****************************** End of making list of new particles ****************************** " << std::endl;
    assert(this->check_state_orders());
    }



/*!
 * ParticleContatiner destructor
 */
ParticleContainer::~ParticleContainer() {}


/*!
 * Proper particleContatiner destructor, remove pointers of the ForestState
 */
void ParticleContainer::clear(){
    // When this is called, this should be the difference between number of forestStates ever built minus ones have already been removed. this should be equal to the size for particles.
     cout<<"Forest state was created " << new_forest_counter << " times" << endl;  // DEBUG
     cout<<"Forest state destructor was called " << delete_forest_counter << " times" << endl; // DEBUG

    dout << "ParticleContainer clear() is called" << endl;
    for (size_t i = 0; i < this->particles.size(); i++){
        if (this->particles[i]!=NULL){
            delete this->particles[i];
            this->particles[i]=NULL;
            }
        }
    this->particles.clear();
    dout << "Particles are deleted" << endl;
    }


/*!
 * Append new ForestState to the end of the ParticleContainer with weight.
 */
void ParticleContainer::push(ForestState* state, double weight){
    state->setParticleWeight(weight);
    this->particles.push_back(state);
    }


/*!
 * @ingroup group_pf_resample
 * @ingroup group_pf_update
 * \brief Calculate the effective sample size, and update the cumulative weight of the particles
 */
void ParticleContainer::update_cum_sum_array_find_ESS(std::valarray<double> & weight_cum_sum){
    double wi_sum=0;
    double wi_sq_sum=0;
    double Num_of_states = this->particles.size();
    weight_cum_sum=0; //Reinitialize the cum sum array

    for (size_t i=0; i < Num_of_states ;i++){
        //update the cum sum array
        double w_i=this->particles[i]->weight();
        weight_cum_sum[i+1]=weight_cum_sum[i]+w_i;
        wi_sum = wi_sum + w_i;
        wi_sq_sum = wi_sq_sum + w_i * w_i;
        }

    //check for the cum weight
    dout << "### particle weights ";
    for (size_t i=0;i<Num_of_states;i++){
        dout << this->particles[i]->weight()<<"  ";
        } dout << std::endl<<std::endl;

    dout << "### updated cum sum of particle weight ";
    for (size_t i=0;i<weight_cum_sum.size();i++){
        dout << weight_cum_sum[i]<<"  ";
        } dout << std::endl;

    this->set_ESS(wi_sum * wi_sum / wi_sq_sum);
    }


/*!
 * Normalize the particle weight, inorder to prevent underflow problem
 */
void ParticleContainer::normalize_probability(){
    double total_probability = 0;
    for ( size_t particle_i = 0;particle_i < this->particles.size(); particle_i++ ){
        total_probability += this->particles[particle_i]->weight();
        }
    for ( size_t particle_i = 0; particle_i < this->particles.size(); particle_i++ ){
        this->particles[particle_i]->setParticleWeight( this->particles[particle_i]->weight() / total_probability);
        }
    }


void ParticleContainer::update_state_to_data( double mutation_rate, double loci_length, Segment * Segfile, valarray<double> & weight_cum_sum ){

    dout <<  " ******************** Update the weight of the particles  ********** " <<endl;
    dout << " ### PROGRESS: update weight at " << Segfile->segment_start()<<endl;
    #ifndef _REJECTION
    //Update weight for seeing mutation at the position
    //Extend ARGs and update weight for not seeing mutations along the equences
    this->extend_ARGs( mutation_rate, (double)min(Segfile->segment_end(), loci_length) , Segfile->segment_state() );

    dout << " Update state weight at a SNP "<<endl;
    this->update_weight_at_site( mutation_rate, Segfile->allelic_state_at_Segment_end );

    #endif
    dout << "Extended until " << this->particles[0]->current_base() <<endl;
    //Update the cumulated probabilities, as well as computing the effective sample size
    this->update_cum_sum_array_find_ESS( weight_cum_sum );
}


/*!
 * @ingroup group_naive
 * \brief Use simple random sampling to resample
 */
void ParticleContainer::trivial_resampling( std::valarray<int> & sample_count, size_t num_state ){
    sample_count=0;
    for (size_t i=0; i < num_state ;i++){
        size_t index = random_generator()->sampleInt(num_state);
        sample_count[index]=sample_count[index]+1;
        }
        //cout << sample_count.sum() <<endl;
        assert( sample_count.sum() == num_state );
    }


/*!
 * @ingroup group_systematic
 * \brief Use systematic resampling \cite Doucet2008 to generate sample count for each particle
 */
void ParticleContainer::systematic_resampling(std::valarray<double> cum_sum, std::valarray<int>& sample_count, int sample_size){
    size_t interval_j = 0;
    size_t sample_i = 0;
    size_t N = sample_size;
    //double u_j = rand() / double(RAND_MAX) / N;
    double u_j = this->random_generator()->sample() / N;
    double cumsum_normalization = cum_sum[cum_sum.size()-1];

    resampledout << "systematic sampling procedure on interval:" << std::endl;
    resampledout << " ";
    for (size_t i=0;i<cum_sum.size();i++){dout <<  (cum_sum[i]/cumsum_normalization )<<"  ";}dout << std::endl;

    sample_count[sample_i] = 0;
    while (sample_i < N) {
        resampledout << "Is " <<  u_j<<" in the interval of " << std::setw(10)<< (cum_sum[interval_j]/ cumsum_normalization) << " and " << std::setw(10)<< (cum_sum[interval_j+1]/ cumsum_normalization) << " ? ";
        /* invariants: */
        assert( (cum_sum[interval_j] / cumsum_normalization) < u_j );
        assert( sample_i < N );
        /* check whether u_j is in the interval [ cum_sum[interval_j], cum_sum[interval_j+1] ) */
        if ( (sample_i == N) || cum_sum[interval_j+1] / cumsum_normalization > u_j ) {
            sample_count[interval_j] += 1;
            sample_i += 1;
            dout << "  yes, update sample count of particle " << interval_j<<" to " << sample_count[interval_j] <<std::endl;
            u_j += 1.0/double(N);
            }
        else {
            dout << "   no, try next interval " << std::endl;
            //assert( sample_i < N-1 );
            interval_j += 1;
            sample_count[ interval_j ] = 0;
            }
        }
    interval_j=interval_j+1;
    for (;interval_j<N;interval_j++){
        sample_count[ interval_j ] = 0;
        }

    resampledout << "systematic sampling procedue finished with total sample count " << sample_count.sum()<<std::endl;
    resampledout << "Sample counts: " ;
    for (size_t i=0;i<sample_count.size();i++){dout << sample_count[i]<<"  ";}  dout << std::endl;
    assert(sample_count.sum() == sample_size);
    }


bool ParticleContainer::appendingStuffToFile( double x_end,  PfParam &pfparam){
    // Record the TMRCA and weight when a heatmap is generated
    if (!pfparam.heat_bool){
        return true;
        }
           /*!
             *  \verbatim
           remove all the state prior to the minimum of
            current_printing_base and
                previous_backbase
                .                      backbase                     Segfile->site()
                .                      .                            .
                .                      .     3                      .
                .                      .     x---o              6   .
                .                  2   .     |   |              x-------o
                .                  x---------o   |              |   .
                .                  |   .         |              |   .
             0  .                  |   .         x---o          |   .
             x---------o           |   .         4   |          |   .
                .      |           |   .             x----------o   .
                .      |           |   .             5              .
                .      x-----------o   .                            .
                .      1               .-------------lag------------.
                .                      .                            .
                Count::update_e_count( .                            ParticleContainer::update_state_to_data(
           x_start = previous_backbase .                              mutation data comes in here
           x_end = backbase            .
                                       .
                                       ParticleContainer::appendingStuffToFile(
                                           x_end = backbase,
          \endverbatim
         *
         * Likelihood of the particle is updated up until state 6, but because of the lagging we are using
         * report the TMRCA up until state 2
         *
         */
    if (x_end < this->current_printing_base()){
        return true;
        }
    do {
        //this->set_current_printing_base(x_end);

        if (this->current_printing_base() > 0){

            ofstream TmrcaOfstream   ( pfparam.TMRCA_NAME.c_str()    , ios::out | ios::app | ios::binary);
            ofstream WeightOfstream  ( pfparam.WEIGHT_NAME.c_str()   , ios::out | ios::app | ios::binary); ;
            //ofstream BLOfstream      ( pfparam.BL_NAME.c_str()       , ios::out | ios::app | ios::binary);;
            ofstream SURVIVORstream  ( pfparam.SURVIVOR_NAME.c_str() , ios::out | ios::app | ios::binary);

            TmrcaOfstream  << this->current_printing_base();
            WeightOfstream << this->current_printing_base();
            //BLOfstream     << this->current_printing_base();
            SURVIVORstream << this->current_printing_base();

            for ( size_t i = 0; i < this->particles.size(); i++){
                ForestState * current_state_ptr = this->particles[i];
                WeightOfstream <<"\t" << current_state_ptr->weight();

                //TmrcaOfstream  << "\t" << current_state_ptr->local_root()->height() / (4 * current_state_ptr->model().default_pop_size); // Normalize by 4N0
                double current_tmrca = current_state_ptr->local_root()->height();
                for (size_t tmrca_i = current_state_ptr->TmrcaHistory.size(); tmrca_i > 0; tmrca_i--){
                    if (current_state_ptr->TmrcaHistory[tmrca_i].base < this->current_printing_base()){
                        break;
                        }
                    current_tmrca = current_state_ptr->TmrcaHistory[tmrca_i].tmrca ;
                    }
                TmrcaOfstream  << "\t" << current_tmrca / (4 * current_state_ptr->model().default_pop_size()); // Normalize by 4N0

                //BLOfstream     << "\t" << current_state_ptr->local_tree_length()    / (4 * current_state_ptr->model().default_pop_size); // Normalize by 4N0
                current_state_ptr=NULL;
                }

            TmrcaOfstream  << endl;
            WeightOfstream << endl;
            //BLOfstream     << endl;
            SURVIVORstream << endl;

            TmrcaOfstream.close();
            WeightOfstream.close();
            //BLOfstream.close();
            SURVIVORstream.close();
            }
        this->set_current_printing_base(this->current_printing_base() + pfparam.heat_seq_window);
        } while ( this->current_printing_base() < x_end);
    return true;
    }


void ParticleContainer::set_particles_with_random_weight(){
    for (size_t i = 0; i < this->particles.size(); i++){
        //this->particles[i]->setParticleWeight( this->random_generator()->sample() );
        this->particles[i]->setParticleWeight( this->particles[i]->random_generator()->sample() );
        }
    }

//// We need to decide at the tail of the data, until the end of the sequence, whether to perform recombination or not, extend arg from the prior? or ?
//void ParticleContainer::cumulate_recomb_opportunity_at_seq_end( double loci_length ){
    //for (size_t i = 0; i < this->particles.size(); i++){
        //this->particles[i]->record_the_final_recomb_opportunity ( loci_length );
    //}
//}


void ParticleContainer::print_particle_probabilities(){
    for (size_t i = 0; i < this->particles.size(); i++){
        cout<<"weight = "<<this->particles[i]->weight()<<endl;
        }
    }


//void ParticleContainer::print_particle_newick(){
    //for (size_t i = 0; i < this->particles.size(); i++){
        //cout << this->particles[i]->scrmTree << ";" << endl;
    //}
//}
