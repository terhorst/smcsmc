/*
 * smcsmc is short for particle filters for ancestral recombination graphs.
 * This is a free software for demographic inference from genome data with particle filters.
 *
 * Copyright (C) 2013-2017 Donna Henderson, Sha (Joe) Zhu and Gerton Lunter
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

#include <climits> // INT_MAX
#include "particle.hpp"
#include "pfparam.hpp"
#include "descendants.hpp"


/*! \brief Initialize a new ForestState.  A copy of model/random_generator is made if own_m_and_rg==True; either way the objects are not stolen
 * @ingroup group_pf_init
 * */
ForestState::ForestState( Model* model,
                          RandomGenerator* random_generator,
                          const vector<int>& record_event_in_epoch,
                          bool own_model_and_random_generator)
            : Forest( model, random_generator ),
              record_event_in_epoch(record_event_in_epoch) {
    /*! Initialize base of a new ForestState, then do nothing, other members will be initialized at an upper level */
    this->setPosteriorWeight( 1.0 );
    this->setPilotWeight( 1.0 );
    this->total_delayed_adjustment = 1.0;
    this->setSiteWhereWeightWasUpdated(0.0);
    owning_model_and_random_generator = own_model_and_random_generator;
    if (owning_model_and_random_generator) {
        // as we're owning it, we should make copies
        size_t new_seed = (size_t)random_generator_->sampleInt( INT_MAX );
        random_generator_ = new MersenneTwister( new_seed , random_generator_->ff() );
        model_ = new Model( *model_ );
    }
}


/*! \brief Create a newly copied ForestState
    @ingroup group_pf_resample
*/
ForestState::ForestState( const ForestState & copied_state )
            :Forest( copied_state ),
             record_event_in_epoch( copied_state.record_event_in_epoch ) {
    setPosteriorWeight( copied_state.posteriorWeight() );
    setPilotWeight( copied_state.pilotWeight() );
    this->total_delayed_adjustment = copied_state.total_delayed_adjustment;
    this->delayed_adjustments = copied_state.delayed_adjustments;
    setSiteWhereWeightWasUpdated( copied_state.site_where_weight_was_updated() );
    copyEventContainers ( copied_state );
    this->current_rec_ = copied_state.current_rec_;
    owning_model_and_random_generator = copied_state.owning_model_and_random_generator;
    if (owning_model_and_random_generator) {
        // as we (and copied_state) own model and rg, we should make copies
        size_t new_seed = (size_t)random_generator_->sampleInt( INT_MAX );
        random_generator_ = new MersenneTwister( new_seed , random_generator_->ff() );
        model_ = new Model( *model_ );
    }

    for (size_t i = 0 ; i < copied_state.TmrcaHistory.size(); i++ ){
        this->TmrcaHistory.push_back(copied_state.TmrcaHistory[i]);
    }
    dout << "current particle's weight is " << this->posteriorWeight()<<endl;
}


void ForestState::copyEventContainers(const ForestState & copied_state ){
    // Copy trees
    for (size_t i=0; i < copied_state.eventTrees.size() ; i++) {
        EvolutionaryEvent* new_event = copied_state.eventTrees[i];
        eventTrees.push_back( new_event );
        if (new_event) {
            new_event->increase_refcount();
        }
    }
}


void ForestState::init_EventContainers( Model * model ) {
    for (size_t i=0; i < model->change_times_.size(); i++) {
        eventTrees.push_back( NULL );
    }
}


/*! \brief Destructor of ForestState
 * Recursively remove all the previous states, if the pointer counter is zero
 */
ForestState::~ForestState() {
    clear_eventContainer();
    if (owning_model_and_random_generator) {
        delete random_generator_;     //MULTITRHREADING
        delete model_;
        random_generator_ = NULL;
        model_ = NULL;
    }
    TmrcaHistory.clear();
    this->contemporaries_.clear();
    this->rec_bases_.clear();
}


/*! Clear coalescent and recombination events recorded between two states.*/
void ForestState::clear_eventContainer(){
    for (int i = eventTrees.size()-1 ; i>=0 ; --i) {
        if (eventTrees[i] && eventTrees[i]->decrease_refcount_is_zero()) {
            // We use placement new, so need to call destructor explicitly.
            // However the destructor should recursively delete its parents,
            // and therefore must know the epoch -- but we can't pass parameters.
            // So, call a helper deleter, that can take a parameter and both
            // destructs and deallocates the memory.
            eventTrees[i]->deletethis( i );  // this recursively deletes its parents
        }
    }
}


void ForestState::record_all_event(TimeIntervalIterator const &ti, double &recomb_opp_x_within_scrm){

    double recomb_opp_x_within_smcsmc = 0; // DEBUG
    double start_base, end_base;
    double start_height, end_height;
    size_t start_height_epoch, end_height_epoch;

    // find extent of time interval where an event may have occurred
    start_height = (*ti).start_height();
    start_height_epoch = ti.forest().model().current_time_idx_;
    if (this->tmp_event_.isNoEvent()) {
        end_height = start_height + (*ti).length();
    } else {
        end_height = this->tmp_event_.time();
        assert (end_height > start_height);
    }
    // interval either runs to event, or to change point; in both cases the end_height has the
    // same epoch as start_height (but call to getTimeIdx will return epoch+1 if end_height ran to end of interval)
    end_height_epoch = start_height_epoch;

    // loop over the two nodes
    for (int i=0; i<2; i++) {

        if (states_[i] == 2) {
            // node i is tracing an existing non-local branch; opportunities for recombination
            if (!(record_event_in_epoch[ writable_model()->current_time_idx_ ] & PfParam::RECORD_RECOMB_EVENT)) continue;
            start_base = get_rec_base(active_node(i)->last_update());
            end_base = this->current_base();
            if (end_base == start_base) continue;
            void* event_mem = Arena::allocate( start_height_epoch );
            EvolutionaryEvent* recomb_event = new(event_mem) EvolutionaryEvent( start_height, start_height_epoch,
                                                                                end_height, end_height_epoch,
                                                                                start_base, end_base, 1 );
            double recomb_pos = -1;
            if (tmp_event_.isRecombination() && tmp_event_.active_node_nr() == i) {
                recomb_pos = (start_base + end_base)/2;   // we should sample from [start,end], but the data isn't used
                recomb_event->set_recomb_event_pos( recomb_pos );
                recomb_event->set_descendants( get_descendants( active_nodes_[i] ) );  // calculate signature for descendants of node
            }
            // add event in tree data structure
            recomb_event->add_leaf_to_tree( &eventTrees[ writable_model()->current_time_idx_] );
            recomb_opp_x_within_smcsmc += end_base - start_base;
            assert(recomb_event->print_event());
        } else if (states_[i] == 1) {
            // node i is tracing out a new branch; opportunities for coalescences and migration
            if (!(record_event_in_epoch[ writable_model()->current_time_idx_ ] & PfParam::RECORD_COALMIGR_EVENT)) continue;
            start_base = this->current_base();
            int weight = ti.contemporaries_->size( active_node(i)->population() );
            // consider normal (not pairwise) coalescences that occurred on this node
            bool coal_event = (tmp_event_.isCoalescence() && tmp_event_.active_node_nr() == i);
            bool migr_event = (tmp_event_.isMigration() && tmp_event_.active_node_nr() == i);
            // account for potential pairwise coalescence opportunity and event
            if (i==0 && states_[1]==1 && active_node(0)->population() == active_node(1)->population()) {
                weight += 1;
                coal_event |= tmp_event_.isPwCoalescence();
            }
            // Record coalescence and migration opportunity (note: if weight=0, there is still opportunity for migration)
            void* event_mem = Arena::allocate( start_height_epoch );
            EvolutionaryEvent* migrcoal_event = new(event_mem) EvolutionaryEvent(start_height, start_height_epoch,
                                                                                 end_height, end_height_epoch,
                                                                                 start_base, active_node(i)->population(), weight);
            if (coal_event) {
                migrcoal_event->set_coal_event();
            }
            if (migr_event) {
                migrcoal_event->set_migr_event( tmp_event_.mig_pop() );
            }
            // add event in tree data structure
            migrcoal_event->add_leaf_to_tree( &eventTrees[ writable_model()->current_time_idx_] );
            assert(migrcoal_event->print_event());
        }
    }
}


void ForestState::record_Recombevent_b4_extension (){

    // iterate over time intervals (but do NOT prune branches at this stage)
    for (TimeIntervalIterator ti(this, this->nodes_.at(0)); ti.good(); ++ti) {
        // Create a recombination event for this slice (which may be smaller than an epoch -- but in our case it usually won't be)
        int contemporaries = ti.contemporaries_->numberOfLocalContemporaries();
        if (contemporaries > 0 && (record_event_in_epoch[ writable_model()->current_time_idx_ ] & PfParam::RECORD_RECOMB_EVENT)) {
            double start_height = (*ti).start_height();
            double end_height = (*ti).end_height();
            size_t start_height_epoch = ti.forest()->model().current_time_idx_;
            //assert( start_height_epoch == ti.forest()->model().getTimeIdx( start_height ) );
            size_t end_height_epoch = start_height_epoch;
            void* event_mem = Arena::allocate( start_height_epoch );
            // no event for now
            EvolutionaryEvent* recomb_event = new(event_mem) EvolutionaryEvent( start_height, start_height_epoch,
                                                                                end_height, end_height_epoch,
                                                                                this->current_base(), this->next_base(), contemporaries );
            recomb_event->add_leaf_to_tree( &eventTrees[ writable_model()->current_time_idx_] );
            //cout << "GENERATE Rw" << contemporaries << " [" << this->current_base() << "," << this->next_base()
            //     << "]x[" << start_height << "," << end_height << "]" << endl;
        }
    }
}


void ForestState::resample_recombination_position(void) {
    // first, obtain a fresh sequence position for the next recombination, overwriting the existing sample in next_base_
    this->resampleNextBase();
    // then, create private event records, in effect re-doing the work of record_Recombevent_b4_extension
    for (int epoch = 0; epoch < eventTrees.size(); epoch++) {
        if (record_event_in_epoch[ epoch ] & PfParam::RECORD_RECOMB_EVENT) {
            EvolutionaryEvent* old_event = eventTrees[ epoch ];      // pointer to old event to be considered for copying
            EvolutionaryEvent** new_chain = &eventTrees[ epoch ];    // ptr to ptr to current event chain
            // break out the loop if no (further) recombination opportunity has been recorded
            if ( !old_event || !old_event->is_recomb() || !old_event->recomb_event_overlaps_opportunity_x( this->current_base() ) ) {
                break;
            }
            do {
                // make a copy of current event and modify the end_base member
                void* event_mem = Arena::allocate( epoch );
                EvolutionaryEvent* new_event = new(event_mem) EvolutionaryEvent( *old_event );
                new_event->end_base_ = this->next_base();        // friend function access
                // splice into event tree, and update pointers
                new_event->add_leaf_to_tree( new_chain );
                new_chain = &(new_event->parent_);              // friend function access
                old_event = old_event->parent();
            } while ( old_event && old_event->is_recomb() && old_event->recomb_event_overlaps_opportunity_x( this->current_base() ) );
        }
    }
}


void ForestState::record_Recombevent_atNewGenealogy ( double event_height )
{
    this->writable_model()->resetTime( event_height );
    size_t epoch_i = this->writable_model()->current_time_idx_;
    if (!(record_event_in_epoch[ epoch_i ] & PfParam::RECORD_RECOMB_EVENT))
        return;
    // find the EvolutionaryEvent to add this event to.
    EvolutionaryEvent* event = eventTrees[ epoch_i ];
    while ( !event->is_recomb() || !event->recomb_event_overlaps_opportunity_t( event_height ) ) {
        event = event->parent();
        assert (event != NULL);
    }
    assert (event->start_base() == this->current_base());
    event->set_recomb_event_time( event_height );
    event->set_descendants( get_descendants( rec_point.base_node() ) );  // calculate signature for descendants subtended by node
}


void ForestState::include_haplotypes_at_tips(vector <int> &haplotypes_at_tips)
{
    Node *leaf = this->nodes()->at(0);
    assert( leaf->in_sample() );
    assert( leaf->label() == 1 );
    leaf->set_mutation_state( haplotypes_at_tips[0] );
    for (size_t j = 1; j < haplotypes_at_tips.size(); j++) {
        do {
            leaf = leaf->next();
        } while (!leaf->in_sample());
        assert( leaf->label() == j+1 );
        leaf->set_mutation_state( haplotypes_at_tips[j] );
    }
}


/*!
 * Calculate the marginal likelihood of a node recursively, let X denote the state of Node *, and Y denote the state of the
 *  first child, and Z denote the state of the second child. Let t1 denote the time from X to Y, and t2 is the time from X to Z.
 *  Let u(t) be the probability function of the no mutation occurs within time t.
 *
  \verbatim
      X
   t1/ \ t2
    Y   Z
  \endverbatim
 *
 *  Suppose that X, Y and Z only take values 0 or 1.
 *  When X=0, consider the likelihood of the tree in four cases of (y,z) pairs (0,0), (0,1), (1,0) and (1,1)
 *  Similarliy X=1, we have four cases too.
 * X[0]=y[0] * ut1 * z[0] * ut2 + y[0] * ut1 * z[1] * (1-ut2) + y[1] * (1-ut1) * z[0] * ut2 + y[1] * (1-ut1) * z[1] * (1-ut2);
 * X[1]=y[1] * ut1 * z[1] * ut2 + y[1] * ut1 * z[0] * (1-ut2) + y[0] * (1-ut1) * z[1] * ut2 + y[0] * (1-ut1) * z[0] * (1-ut2);
 *
 * After simplification,
 * X[0]=(y[0]*ut1 + y[1]*(1-ut1)) * (z[0]*ut2 + z[1]*(1-ut2))
 * X[1]=(y[1]*ut1 + y[0]*(1-ut1)) * (z[1]*ut2 + z[0]*(1-ut2))
 *
 * Note: even though there are only two states, x[0]+x[1] is not 1!!! becasue x[0] is a marginal probability, but still conditional on all the possiblities of the variants
 *
 *  If X, Y and Z take values A, T, C and G. The computation would be more complex.  Four equations, with 16 terms in each.
 *
 * If we consider JC69 model, then
 * X[0]= y[0] * ut1 * z[0] * ut2  + y[0] * ut1 * sum(z[1,2,3]) * (1-ut2) +  sum(y[1,2,3]) * (1-ut1) * z[0] * ut2 + sum(y[1,2,3]) * (1-ut1) * sum(z[1,2,3]) * (1-ut2)
 * ...
 *
 * @ingroup group_resample
 * * */


inline valarray<double> ForestState::cal_partial_likelihood_infinite(Node * node) {

    valarray<double> part_lik(2);    // partial likelihood of data under this node conditional on current node being in state 0 or 1

    // deal with the case that this node is a leaf node
    if ( node->first_child() == NULL ) {
      assert ( node->mutation_state() != 2);
      assert ( node->second_child() == NULL );          // if a node has no first child, it won't have a second child
      assert ( node->in_sample() );                     // we only traverse the local tree, therefore the leaf node must be in our sample
      part_lik[0] = node->mutation_state() == 1 ? 0.0 : 1.0;    // also encode state==-1 (missing data) as 1.0
      part_lik[1] = node->mutation_state() == 0 ? 0.0 : 1.0;   // also encode state==-1 (missing data) as 1.0
      return part_lik;
    }

    // Genealogy branch lengths are in number of generations, the mutation rate is unit of per site per generation, often in the magnitude of 10 to the power of negative 8.
    double mutation_rate = this->model().mutation_rate();
    Node *left = trackLocalNode(node->first_child());   // jz_stable; for jz use Node *left = node->getLocalChild1();
    Node *right = trackLocalNode(node->second_child()); // jz_stable; for jz use Node *right = node->getLocalChild2();
    double t_left = node->height() - left->height();
    double t_right = node->height() - right->height();
    assert (t_left >= 0 && t_right >= 0 && mutation_rate > 0);
    double p_left = exp(-t_left * mutation_rate);   // probability that no mutation occurred along left branch.  Assume infinite sites model
    double p_right = exp(-t_right * mutation_rate); // probability that no mutation occurred along left branch.  Assume infinite sites model
    valarray<double> part_lik_left = cal_partial_likelihood_infinite(left);     // partial likelihoods of data under left and right children
    valarray<double> part_lik_right = cal_partial_likelihood_infinite(right);

    part_lik[0] = ( part_lik_left[0]*p_left + part_lik_left[1]*(1-p_left) ) * ( part_lik_right[0]*p_right + part_lik_right[1]*(1-p_right) );
    part_lik[1] = ( part_lik_left[1]*p_left + part_lik_left[0]*(1-p_left) ) * ( part_lik_right[1]*p_right + part_lik_right[0]*(1-p_right) );

    return part_lik;

}


/*!
 * \brief Calculate the marginal likelihood of the genealogy at data site i,
 *  If there is no data given at the site i, return likelihood as 1.
 * @ingroup group_pf_resample
 */
double ForestState::calculate_likelihood( ) {
    valarray<double> marginalLikelihood = cal_partial_likelihood_infinite(this->local_root());
    double prior[2] = {0.5,0.5};
    double likelihood = marginalLikelihood[0]*prior[0] + marginalLikelihood[1]*prior[1];
    return likelihood;
}


double ForestState::trackLocalTreeBranchLength() {
    BranchLengthData bld = trackSubtreeBranchLength( this->local_root() );
    if (bld.subtreeBranchLength == -1) {
        // none of the leaves carry data -- total length is 0
        return 0;
    }
    // return branch length of the subtree subtending leaves carrying data
    return bld.subtreeBranchLength;
}


Node* ForestState::trackLocalNode(Node *node) const {
  assert( node->local() );
  if (node->countChildren() == 0) return node;
  if (node->countChildren() == 1) return trackLocalNode(node->first_child());

  assert( node->countChildren() == 2 );
  assert( node->first_child()->local() || node->second_child()->local() );

  if ( node->first_child()->local() ) {
    if (node->second_child()->local()) return node;
    else return trackLocalNode(node->first_child());
  }
  else return trackLocalNode(node->second_child());
}


BranchLengthData ForestState::trackSubtreeBranchLength ( Node * currentNode ) {

    if (currentNode->in_sample() ) {
        // current node is a leaf node
        if (currentNode->mutation_state() >= 0) {
            // leaf node carries data
            return BranchLengthData( 0, 0 );
        } else {
            // leaf node carries no data
            return BranchLengthData( 0, -1 );
        }
    }

    Node* left_local_child = trackLocalNode(currentNode->first_child());
    Node* right_local_child = trackLocalNode(currentNode->second_child());

    BranchLengthData bld_left  = this->trackSubtreeBranchLength( left_local_child );
    BranchLengthData bld_right = this->trackSubtreeBranchLength( right_local_child );

    // calculate branch length of partial tree, including the branch from this node to the child node
    double leftBL = bld_left.partialBranchLength + (currentNode->height() - left_local_child->height());
    double rightBL = bld_right.partialBranchLength + (currentNode->height() - right_local_child->height());

    // return correct partial tree branch length, and subtree branch length.  The calculation depends on
    // whether left and right subtrees carry data or not.
    if (bld_left.subtreeBranchLength >= 0 && bld_right.subtreeBranchLength >= 0)
        // both left and right subtrees carry data, so current node is a possible root node
        return BranchLengthData( leftBL+rightBL, leftBL+rightBL );

    if (bld_left.subtreeBranchLength >= 0)
        // left subtree carries data, but right one doesn't -- keep left root as possible root node
        return BranchLengthData( leftBL, bld_left.subtreeBranchLength );

    if (bld_right.subtreeBranchLength >= 0)
        // same for right subtree
        return BranchLengthData( rightBL, bld_right.subtreeBranchLength );

    // neither subtree contains data, so just return the length data of either
    return bld_left;
}


double ForestState::extend_ARG ( double mutation_rate, double extend_to, bool updateWeight, bool recordEvents ) {

    double updated_to = this->site_where_weight_was_updated();
    assert (updated_to >= this->current_base());
    // the likelihood of the data along the segment [updated_to, extend_to) given the ARG
    double likelihood = 1.0;
    // the ratio of the prob density of the ARG under the transition part
    // of the pilot distribution, to that under the simulation distribution
    double imp_weight_simulation_to_pilot_dist = 1.0;

    while ( updated_to < extend_to ) {
        /*!
         * First, update the likelihood up to either extend_to or the end of this state
         */
        double update_to = min( extend_to, this->next_base() );
        // calculate the total tree length of the subtree over leaf nodes that carry data.
        double localTreeBranchLength = this->trackLocalTreeBranchLength();
        // for leaf nodes that carry no data, there is no evidence for presence or absence of mutations on corresponding branches.
        // This is accounted for in trackLocalTreeBranchLength.

        double likelihood_of_segment = exp( -mutation_rate * localTreeBranchLength * (update_to - updated_to) );
        likelihood *= likelihood_of_segment;

        if(model().biased_sampling) {
            // Update importance sampling correction for the weight of the particle.
            // Positional importance factors are stored in importance_weight_predata and applied to
            // the particle weight with the likelihood at the end of extend_ARG().

            bool sampled_recombination = ((update_to < extend_to) && (update_to > model().getCurrentSequencePosition() ));
            imp_weight_simulation_to_pilot_dist *=
                imp_weight_simulation_to_pilot_dist_over_segment( updated_to, update_to, sampled_recombination );
        }

        updated_to = update_to;                // rescues the invariant

        /*!
         * Next, if we haven't reached extend_to now, add a new state and iterate
         */
        if ( updated_to < extend_to ) {
            // a recombination has occurred (or the recombination rate has changed)
            double rec_height = this->sampleNextGenealogy( recordEvents );
            this->sampleRecSeqPosition( recordEvents );

            if (recordEvents) {
                this->record_Recombevent_b4_extension();
                if (rec_height > 0.0) {
                    // actual recombination -- record it
                    this->record_Recombevent_atNewGenealogy(rec_height);
                }
            }

        #ifdef _SCRM
            double segment_length_ = this->calcSegmentLength();
            if ( segment_length_ > 1.0 ){ //  only print tree, whose segment length is at least 1
                cout << this->newick(this->local_root()) << ";" << endl;
            }
        #endif

        }

        this->set_current_base( updated_to );  // record current position, to enable resampling of recomb. position

    }
    assert (updated_to == extend_to);
    if (updateWeight) {
        // update weights for extension of ARG
        this->setPosteriorWeight( this->posteriorWeight() * likelihood * imp_weight_simulation_to_pilot_dist );
        if (model().biased_sampling) {

            this->setPilotWeight( this->pilotWeight() * likelihood * imp_weight_simulation_to_pilot_dist );
            assert( std::abs(this->posteriorWeight() - this->pilotWeight() * this->total_delayed_adjustment) <= .001 * this->posteriorWeight() );

        // update weights for application positions passed during extension
            if( !delayed_adjustments.empty() ) {
                while ( !delayed_adjustments.empty() && delayed_adjustments.top().application_position < extend_to) {

                    total_delayed_adjustment = total_delayed_adjustment / delayed_adjustments.top().importance_factor;
                    this->setPilotWeight( this->pilotWeight() * delayed_adjustments.top().importance_factor );
                    delayed_adjustments.pop();

                    assert( std::abs(this->posteriorWeight() - this->pilotWeight() * this->total_delayed_adjustment) <= .001 * this->posteriorWeight() );
                }
            }
        }
    }
    this->setSiteWhereWeightWasUpdated( extend_to );
    return likelihood;
}


std::string ForestState::newick(Node *node) {
  std::stringstream tree;
  if (node->in_sample()) tree << node->label();
  else {
    Node *left = node->getLocalChild1();
    Node *right = node->getLocalChild2();

    tree << "(" << this->newick(left) << ":" <<
           (node->height() - left->height()) * this->model().scaling_factor() <<
           "," << this->newick(right) << ":" <<
           (node->height() - right->height()) * this->model().scaling_factor() << ")";
  }

  return tree.str();
}


/**
 * Function for sampling the sequence position of the next recombination event
 *
 */

void ForestState::sampleRecSeqPosition( bool recordEvents ) {

  if( model().biased_sampling ) {

    this->sampleBiasedRecSeqPosition( recordEvents );
    return;
  }

  this->sampleNextBase();

  assert( this->printTree() );
  this->calcSegmentSumStats();

}


//// biased sampling

/**
 * Function for calculating the weighted branch length above a node.
 * The portion of the branch in time section i is weighted by bias_ratio[i]
 * NB: bias_heights is the outer boundaries and hence has one more element than bias_ratios (e.g. bh=(0,200,DBL_MAX), br=(5,0.5))
 *
 *  \return the weighted length of the branch above node.
 */
double ForestState::WeightedBranchLengthAbove( Node* node ) const {

    // identify time section of the bottom of the branch (the node)
    size_t node_time_idx = 0;
    while ( model().bias_heights()[ node_time_idx+1 ] <= node->height() ) {
        node_time_idx++;
    }

    // identify the time section of the top of the branch (the parent node)
    size_t parent_node_time_idx = node_time_idx;
    while ( model().bias_heights()[ parent_node_time_idx+1 ] <= node->parent_height() ) {
        parent_node_time_idx++;
    }

    // loop over time sections present in branch and add the weighted length
    double weighted_branch_length = 0;
    for ( size_t time_idx = node_time_idx; time_idx <= parent_node_time_idx; time_idx++) {
        double lower_end = max( model().bias_heights()[time_idx] , node->height() );
        double upper_end = min( model().bias_heights()[time_idx+1] , node->parent_height() );
        assert( upper_end >= lower_end );
        weighted_branch_length += model().bias_ratios()[time_idx] * ( upper_end - lower_end );
    }

    return (weighted_branch_length);
}

/**
 * Function to get the weighted tree length for the purpose of 'uniformly'
 * sampling on the tree accounting for the bias.
 *
 * Look through the non-root nodes and account for WeightedBranchLengthAbove
 *
 * \return the sum of weighted branches
 */
double ForestState::getWeightedLocalTreeLength() const {
    return getWeightedLengthBelow( local_root() );
}

/**
 * Function to recursively calculate the weighted length below a node.
 *
 * \return the sum of weighted branches below node
 */

double ForestState::getWeightedLengthBelow( Node* node ) const {
    double weighted_length = 0;

    // for all children, add length between node and child, and length below child

    if ( node->first_child() != NULL && node->first_child()->samples_below() > 0 ) {
        // add length above child
    weighted_length += WeightedBranchLengthAbove(node->first_child());
    // add length below child
    weighted_length += getWeightedLengthBelow(node->first_child());
    }

    if ( node->second_child() !=NULL && node->second_child()->samples_below() > 0 ) {
    // add length above child
    weighted_length += WeightedBranchLengthAbove(node->second_child());
    // add length below child
    weighted_length += getWeightedLengthBelow(node->second_child());
    }

    return weighted_length;
}

/**
 * Function for converting a node and the weighted (sampled) length above
 * to an absolute height. The returned height is found by measuring down from
 * the parent node height in order to stay consistent with SCRM.
 *
 * \return a standardized height
 */
double ForestState::WeightedToUnweightedHeightAbove( Node* node, double length_left) const {

    assert( length_left <= WeightedBranchLengthAbove( node ) );

    // identify time section of the bottom of the branch (the node)
    size_t node_time_idx = 0;
    while ( model().bias_heights()[ node_time_idx+1 ] <= node->height() ) {
        node_time_idx++;
    }

    // identify the time section of the top of the branch (the parent node)
    size_t parent_node_time_idx = node_time_idx;
    while ( model().bias_heights()[ parent_node_time_idx+1 ] <= node->parent_height() ) {
        parent_node_time_idx++;
    }

    // loop over time sections present in branch (starting at the top) and subtract from length_left until the sampled height is reached
    for ( size_t time_idx = parent_node_time_idx; time_idx >= node_time_idx; --time_idx) {
        double lower_end = max( model().bias_heights()[time_idx] , node->height() );
        double upper_end = min( model().bias_heights()[time_idx+1] , node->parent_height() );
        assert( upper_end > lower_end );
        if( length_left >= model().bias_ratios()[time_idx] * (upper_end-lower_end) ) {
            length_left -= model().bias_ratios()[time_idx] * (upper_end-lower_end);
        } else {
            return upper_end - length_left / model().bias_ratios()[time_idx];
        }
    }
    throw std::runtime_error("This should never happen...");
}


/**
 * Function for adjusting the importance weight predata to account for the probability of hitting
 *  a recombination at this sequence position
 *
 * Note: This assumes any change in the recombination rate is imposed in our sampler
 *       and not part of the transition distribution. We should add a feature for a
 *       known recombination map; we'll need to compare inferred_model and proposal_model for this
 */
double ForestState::imp_weight_simulation_to_pilot_dist_over_segment( double previously_updated_to, double update_to, bool sampled_recombination) {
    double sequence_distance_without_rec = update_to - previously_updated_to;
    double transition_prob, proposal_prob;
    if (!sampled_recombination) {
        // We have NOT sampled a recombination at this position
        transition_prob = compute_positional_component_of_transitional_prob_of_no_recombination( sequence_distance_without_rec );
        proposal_prob = compute_positional_component_of_proposal_prob_of_no_recombination( sequence_distance_without_rec );
    } else {
        // We have sampled a recombination at this position
        transition_prob = compute_positional_component_of_transitional_prob_of_recombination( sequence_distance_without_rec );
        proposal_prob = compute_positional_component_of_proposal_prob_of_recombination( sequence_distance_without_rec );
    }
    return transition_prob / proposal_prob;
}

double ForestState::compute_positional_component_of_transitional_prob_of_no_recombination( double sequence_distance_without_rec ) {
    /// !!Temporarily set true recombination rate here, need to decide which class should own this
    // Should create an inferred_model to refer to
    double true_recombination_rate = model().recombination_rate();
    ///
    return std::exp( - sequence_distance_without_rec * true_recombination_rate * getLocalTreeLength() );
}

double ForestState::compute_positional_component_of_proposal_prob_of_no_recombination( double sequence_distance_without_rec ) {
    return std::exp( - sequence_distance_without_rec * model().recombination_rate() * getWeightedLocalTreeLength() );
}

double ForestState::compute_positional_component_of_transitional_prob_of_recombination( double sequence_distance_without_rec ) {
    /// !!Temporarily set true recombination rate here, need to decide which class should own this
    // Should create an inferred_model to refer to
    double true_recombination_rate = model().recombination_rate();
    ///
    return std::exp( - sequence_distance_without_rec * true_recombination_rate * getLocalTreeLength() ) *
                                 ( 1 - std::exp( - true_recombination_rate * getLocalTreeLength() ) );
}

double ForestState::compute_positional_component_of_proposal_prob_of_recombination( double sequence_distance_without_rec ) {
    return std::exp( - sequence_distance_without_rec * model().recombination_rate() * getWeightedLocalTreeLength() ) *
                                 ( 1 - std::exp( - model().recombination_rate() * getWeightedLocalTreeLength() ) );
}


/**
 * Function for sampling the sequence position of the next recombination event
 * under a biased sampling procedure.
 *
 * want to change importance weight member, rather than returning below
 * \return the importance weight adjustor needed to correct for biased sampling.
 *
 */
void ForestState::sampleBiasedRecSeqPosition( bool recordEvents ) {
    double length = random_generator()->sampleExpoLimit(model().recombination_rate() * getWeightedLocalTreeLength(),
                                                      model().getNextSequencePosition() - current_base());
  if (length == -1) {
    // No recombination until the model changes
    set_next_base(model().getNextSequencePosition());
    if (next_base() < model().loci_length()) writable_model()->increaseSequencePosition();
  } else {
    // Recombination in the sequence segment
    set_next_base(current_base() + length);
  }

  assert(next_base() > current_base());
  assert(next_base() <= model().loci_length());
}


/**
 * Uniformly samples a TreePoint on the local tree.
 *
 * Its arguments are meant to be used only when the function iteratively calls
 * itself. Just call it without any arguments if you want to sample a TreePoint.
 *
 * The function first samples a part of the total height of the tree and then
 * goes down from the root, deciding at each node if that point is to the left
 * or right, which should give us an O(log(#nodes)) algorithm.
 *
 * I checked the distribution of this function in multiple cases. -Paul
 *
 * \param node The current position in the tree when the functions goes down
 *             iteratively.
 *
 * \param length_left The length that is left until we encounter the sampled
 *              length.
 *
 * \return The sampled point on the tree.
 */
TreePoint ForestState::sampleBiasedPoint(Node* node, double length_left) {

    assert(model().biased_sampling);

  if (node == NULL) {
    // Called without arguments => initialization
    assert( this->checkTreeLength() );

    node = this->local_root();
    length_left = random_generator()->sample() * getWeightedLocalTreeLength();
    assert( 0 < length_left && length_left < getWeightedLocalTreeLength() );
  }

  assert( node->local() || node == this->local_root() );
  assert( length_left >= 0 );
  assert( length_left < (getWeightedLengthBelow(node) + WeightedBranchLengthAbove(node)) );

  if ( node != this->local_root() ) {
    if ( length_left < WeightedBranchLengthAbove(node) ) {
      assert( node->local() );
      dout << " before IS_TP the weight is " << this->posteriorWeight() << endl;
      this->IS_TreePoint_adjustor( TreePoint(node, WeightedToUnweightedHeightAbove( node, length_left), false) );
      dout << " Straight after IS_TP the weight is " << this->posteriorWeight() << endl;
      //this is the end of iterating through nodes, so we update here
      return TreePoint(node, WeightedToUnweightedHeightAbove( node, length_left), false);
    }

    length_left -= WeightedBranchLengthAbove(node);
    assert( length_left >= 0 );
  }

  // At this point, we should have at least one local child
  assert( node->first_child() != NULL );
  assert( node->first_child()->local() || node->second_child()->local() );

  // If we have only one local child, then give it the full length we have left.
  if ( !node->first_child()->local() ) {
    return sampleBiasedPoint(node->second_child(), length_left);
  }
  if ( node->second_child() == NULL || !node->second_child()->local() ) {
    return sampleBiasedPoint(node->first_child(), length_left);
  }

  // If we have two local children, the look if we should go down left or right.
  double tmp = WeightedBranchLengthAbove(node->first_child()) + getWeightedLengthBelow(node->first_child());
  if ( length_left <= tmp )
    return sampleBiasedPoint(node->first_child(), length_left);
  else
    return sampleBiasedPoint(node->second_child(), length_left - tmp);
}


/**
 * Function for adjusting the particle weight and the delayed adjustments prioirty queue
 * of weights to be applied to the delayed particle weight at the specified sequence position
 */
void ForestState::IS_TreePoint_adjustor(const TreePoint & rec_point) {

    // figure out the epoch of the recombination event;
    // use the top epoch in case the recombination occurred above the top epoch
    size_t indx = 0;
    while (indx+1 < model().change_times().size() && rec_point.height() >= model().change_times().at(indx+1) ) {
        indx++;
    }

    // find the bias time section containing the rec point height, in order to use the appropriate bias ratio
    size_t time_section_idx = 0;
    while( model().bias_heights()[time_section_idx+1] < rec_point.height() ){
        time_section_idx++;
    }
    assert( time_section_idx <= model().bias_ratios().size() );

    // calculate the importance weight: the ratio of the desired probability density of sampling this time point
    // ( 1 / localTreeLength ) over the probability density of the actual sampling distribution that
    // was used ( bias_ratio / weightedLocalTreeLength ).
    double bias_ratio = model().bias_ratios()[ time_section_idx ];
    double importance_weight = getWeightedLocalTreeLength() / ( bias_ratio * getLocalTreeLength() );

    // update the particle weight so we have a correctly weighted sample of the target distribution
    setPosteriorWeight( posteriorWeight() * importance_weight );

    // delay applying the importance weight to the distribution used for resampling particles, by
    // storing it in the priority queue
    dout << " delayed_adjustments already has " << delayed_adjustments.size() << " elements" << endl;
    delayed_adjustments.push(
        DelayedFactor (
            current_base() + model().application_delays.at(indx) ,
            importance_weight
        )
    );

    // update the total delayed adjustment (for checking only)
    total_delayed_adjustment *= importance_weight;
    assert( std::abs(posteriorWeight() - pilotWeight() * total_delayed_adjustment) <= .001 * posteriorWeight() );
}

