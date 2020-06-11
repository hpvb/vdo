/*
 * Copyright (c) 2020 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/uds-releases/krusty/src/uds/index.c#20 $
 */

#include "index.h"

#include "hashUtils.h"
#include "indexCheckpoint.h"
#include "indexInternals.h"
#include "logger.h"

static const uint64_t NO_LAST_CHECKPOINT = UINT_MAX;


/**
 * Replay an index which was loaded from a checkpoint.
 *
 * @param index			   The index to replay
 * @param last_checkpoint_chapter  The number of the chapter where the
 *				   last checkpoint was made
 *
 * @return UDS_SUCCESS or an error code.
 **/
static int replay_index_from_checkpoint(struct index *index,
					uint64_t last_checkpoint_chapter)
{
	// Find the volume chapter boundaries
	uint64_t lowest_vcn, highest_vcn;
	bool is_empty = false;
	IndexLookupMode old_lookup_mode = index->volume->lookupMode;
	index->volume->lookupMode = LOOKUP_FOR_REBUILD;
	int result = findVolumeChapterBoundaries(index->volume,
						 &lowest_vcn,
						 &highest_vcn,
						 &is_empty);
	index->volume->lookupMode = old_lookup_mode;
	if (result != UDS_SUCCESS) {
		return logFatalWithStringError(result,
					       "cannot replay index: unknown volume chapter boundaries");
	}
	if (lowest_vcn > highest_vcn) {
		logFatal("cannot replay index: no valid chapters exist");
		return UDS_CORRUPT_COMPONENT;
	}

	if (is_empty) {
		// The volume is empty, so the index should also be empty
		if (index->newest_virtual_chapter != 0) {
			logFatal("cannot replay index from empty volume");
			return UDS_CORRUPT_COMPONENT;
		}
		return UDS_SUCCESS;
	}

	unsigned int chapters_per_volume =
		index->volume->geometry->chapters_per_volume;
	index->oldest_virtual_chapter = lowest_vcn;
	index->newest_virtual_chapter = highest_vcn + 1;
	if (index->newest_virtual_chapter ==
	    lowest_vcn + chapters_per_volume) {
		// skip the chapter shadowed by the open chapter
		index->oldest_virtual_chapter++;
	}

	uint64_t first_replay_chapter = last_checkpoint_chapter;
	if (first_replay_chapter < index->oldest_virtual_chapter) {
		first_replay_chapter = index->oldest_virtual_chapter;
	}
	return replay_volume(index, first_replay_chapter);
}

/**********************************************************************/
static int load_index(struct index *index, bool allow_replay)
{
	bool replay_required = false;

	int result = load_index_state(index->state, &replay_required);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (replay_required && !allow_replay) {
		return logErrorWithStringError(UDS_INDEX_NOT_SAVED_CLEANLY,
					       "index not saved cleanly: open chapter missing");
	}

	uint64_t last_checkpoint_chapter =
		((index->last_checkpoint != NO_LAST_CHECKPOINT) ?
			 index->last_checkpoint :
			 0);

	logInfo("loaded index from chapter %" PRIu64 " through chapter %" PRIu64,
		index->oldest_virtual_chapter,
		last_checkpoint_chapter);

	if (replay_required) {
		result = replay_index_from_checkpoint(index,
						      last_checkpoint_chapter);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	unsigned int i;
	for (i = 0; i < index->zone_count; i++) {
		setActiveChapters(index->zones[i]);
	}

	index->loaded_type = replay_required ? LOAD_REPLAY : LOAD_LOAD;
	return UDS_SUCCESS;
}

/**********************************************************************/
static int rebuild_index(struct index *index)
{
	// Find the volume chapter boundaries
	uint64_t lowest_vcn, highest_vcn;
	bool is_empty = false;
	IndexLookupMode old_lookup_mode = index->volume->lookupMode;
	index->volume->lookupMode = LOOKUP_FOR_REBUILD;
	int result = findVolumeChapterBoundaries(index->volume, &lowest_vcn,
						 &highest_vcn, &is_empty);
	index->volume->lookupMode = old_lookup_mode;
	if (result != UDS_SUCCESS) {
		return logFatalWithStringError(result,
					       "cannot rebuild index: unknown volume chapter boundaries");
	}
	if (lowest_vcn > highest_vcn) {
		logFatal("cannot rebuild index: no valid chapters exist");
		return UDS_CORRUPT_COMPONENT;
	}

	if (is_empty) {
		index->newest_virtual_chapter =
			index->oldest_virtual_chapter = 0;
	} else {
		unsigned int num_chapters =
			index->volume->geometry->chapters_per_volume;
		index->newest_virtual_chapter = highest_vcn + 1;
		index->oldest_virtual_chapter = lowest_vcn;
		if (index->newest_virtual_chapter ==
		    (index->oldest_virtual_chapter + num_chapters)) {
			// skip the chapter shadowed by the open chapter
			index->oldest_virtual_chapter++;
		}
	}

	if ((index->newest_virtual_chapter - index->oldest_virtual_chapter) >
	    index->volume->geometry->chapters_per_volume) {
		return logFatalWithStringError(UDS_CORRUPT_COMPONENT,
					       "cannot rebuild index: volume chapter boundaries too large");
	}

	setMasterIndexOpenChapter(index->master_index, 0);
	if (is_empty) {
		index->loaded_type = LOAD_EMPTY;
		return UDS_SUCCESS;
	}

	result = replay_volume(index, index->oldest_virtual_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	unsigned int i;
	for (i = 0; i < index->zone_count; i++) {
		setActiveChapters(index->zones[i]);
	}

	index->loaded_type = LOAD_REBUILD;
	return UDS_SUCCESS;
}

/**********************************************************************/
int make_index(struct index_layout *layout,
	       const struct configuration *config,
	       const struct uds_parameters *user_params,
	       unsigned int zone_count,
	       enum load_type load_type,
	       IndexLoadContext *load_context,
	       struct index **new_index)
{
	struct index *index;
	int result = allocate_index(layout, config, user_params, zone_count,
				    load_type, &index);
	if (result != UDS_SUCCESS) {
		return logErrorWithStringError(result,
					       "could not allocate index");
	}

	index->load_context = load_context;

	uint64_t nonce = get_volume_nonce(layout);
	result = makeMasterIndex(config, zone_count, nonce,
				 &index->master_index);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return logErrorWithStringError(result,
					       "could not make master index");
	}

	result = add_index_state_component(index->state, MASTER_INDEX_INFO,
					    NULL, index->master_index);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}

	result = add_index_state_component(index->state,
					   &INDEX_PAGE_MAP_INFO,
					   index->volume->indexPageMap,
					   NULL);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}

	result = make_chapter_writer(index, get_index_version(layout),
				     &index->chapter_writer);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}

	if ((load_type == LOAD_LOAD) || (load_type == LOAD_REBUILD)) {
		if (!index->existed) {
			free_index(index);
			return UDS_NO_INDEX;
		}
		result = load_index(index, load_type == LOAD_REBUILD);
		switch (result) {
		case UDS_SUCCESS:
			break;
		case ENOMEM:
			// We should not try a rebuild for this error.
			logErrorWithStringError(result,
						"index could not be loaded");
			break;
		default:
			logErrorWithStringError(result,
						"index could not be loaded");
			if (load_type == LOAD_REBUILD) {
				result = rebuild_index(index);
				if (result != UDS_SUCCESS) {
					logErrorWithStringError(result,
								"index could not be rebuilt");
				}
			}
			break;
		}
	} else {
		index->loaded_type = LOAD_CREATE;
		discard_index_state_data(index->state);
	}

	if (result != UDS_SUCCESS) {
		free_index(index);
		return logUnrecoverable(result, "fatal error in make_index");
	}

	if (index->load_context != NULL) {
		lockMutex(&index->load_context->mutex);
		index->load_context->status = INDEX_READY;
		// If we get here, suspend is meaningless, but notify any
		// thread trying to suspend us so it doesn't hang.
		broadcastCond(&index->load_context->cond);
		unlockMutex(&index->load_context->mutex);
	}

	index->has_saved_open_chapter = index->loaded_type == LOAD_LOAD;
	*new_index = index;
	return UDS_SUCCESS;
}

/**********************************************************************/
void free_index(struct index *index)
{
	if (index == NULL) {
		return;
	}
	free_chapter_writer(index->chapter_writer);

	if (index->master_index != NULL) {
		freeMasterIndex(index->master_index);
	}
	release_index(index);
}

/**********************************************************************/
int save_index(struct index *index)
{
	wait_for_idle_chapter_writer(index->chapter_writer);
	int result = finish_checkpointing(index);
	if (result != UDS_SUCCESS) {
		logInfo("save index failed");
		return result;
	}
	begin_save(index, false, index->newest_virtual_chapter);

	result = save_index_state(index->state);
	if (result != UDS_SUCCESS) {
		logInfo("save index failed");
		index->last_checkpoint = index->prev_checkpoint;
	} else {
		index->has_saved_open_chapter = true;
		logInfo("finished save (vcn %" PRIu64 ")",
			index->last_checkpoint);
	}
	return result;
}

/**
 * Get the zone for a request.
 *
 * @param index The index
 * @param request The request
 *
 * @return The zone for the request
 **/
static struct index_zone *get_request_zone(struct index *index,
					   Request *request)
{
	return index->zones[request->zoneNumber];
}

/**
 * Search an index zone. This function is only correct for LRU.
 *
 * @param zone		    The index zone to query.
 * @param request	    The request originating the query.
 *
 * @return UDS_SUCCESS or an error code
 **/
static int search_index_zone(struct index_zone *zone, Request *request)
{
	MasterIndexRecord record;
	int result = getMasterIndexRecord(zone->index->master_index,
					  &request->chunkName, &record);
	if (result != UDS_SUCCESS) {
		return result;
	}

	bool found = false;
	if (record.isFound) {
		result = getRecordFromZone(zone, request, &found,
					   record.virtualChapter);
		if (result != UDS_SUCCESS) {
			return result;
		}
		if (found) {
			request->location =
				computeIndexRegion(zone,
						   record.virtualChapter);
		}
	}

	/*
	 * If a record has overflowed a chapter index in more than one chapter
	 * (or overflowed in one chapter and collided with an existing record),
	 * it will exist as a collision record in the master index, but
	 * we won't *find it in the volume. This case needs special handling.
	 */
	bool overflow_record =
		(record.isFound && record.isCollision && !found);
	uint64_t chapter = zone->newestVirtualChapter;
	if (found || overflow_record) {
		if ((request->action == REQUEST_QUERY) &&
		    (!request->update || overflow_record)) {
			/* This is a query without update, or with nothing to
			 * update */
			return UDS_SUCCESS;
		}

		if (record.virtualChapter != chapter) {
			/*
			 * Update the master index to reference the new chapter
			 * for the block. If the record had been deleted or
			 * dropped from the chapter index, it will be back.
			 */
			result = setMasterIndexRecordChapter(&record, chapter);
		} else if (request->action != REQUEST_UPDATE) {
			/* The record is already in the open chapter, so we're
			 * done */
			return UDS_SUCCESS;
		}
	} else {
		// The record wasn't in the master index, so check whether the
		// name is in a cached sparse chapter.
		if (!isMasterIndexSample(zone->index->master_index,
					 &request->chunkName) &&
		    is_sparse(zone->index->volume->geometry)) {
			// Passing UINT64_MAX triggers a search of the entire
			// sparse cache.
			result = searchSparseCacheInZone(zone, request,
							 UINT64_MAX, &found);
			if (result != UDS_SUCCESS) {
				return result;
			}

			if (found) {
				request->location = LOC_IN_SPARSE;
			}
		}

		if (request->action == REQUEST_QUERY) {
			if (!found || !request->update) {
				// This is a query without update or for a new
				// record, so we're done.
				return UDS_SUCCESS;
			}
		}

		/*
		 * Add a new entry to the master index referencing the open
		 * chapter. This needs to be done both for new records, and for
		 * records from cached sparse chapters.
		 */
		result = putMasterIndexRecord(&record, chapter);
	}

	if (result == UDS_OVERFLOW) {
		/*
		 * The master index encountered a delta list overflow.	The
		 * condition was already logged. We will go on without adding
		 * the chunk to the open chapter.
		 */
		return UDS_SUCCESS;
	}

	if (result != UDS_SUCCESS) {
		return result;
	}

	struct uds_chunk_data *metadata;
	if (!found || (request->action == REQUEST_UPDATE)) {
		// This is a new record or we're updating an existing record.
		metadata = &request->newMetadata;
	} else {
		// This is a duplicate, so move the record to the open chapter
		// (for LRU).
		metadata = &request->oldMetadata;
	}
	return putRecordInZone(zone, request, metadata);
}

/**********************************************************************/
static int remove_from_index_zone(struct index_zone *zone, Request *request)
{
	MasterIndexRecord record;
	int result = getMasterIndexRecord(zone->index->master_index,
					  &request->chunkName, &record);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (!record.isFound) {
		// The name does not exist in master index, so there is nothing
		// to remove.
		return UDS_SUCCESS;
	}

	if (!record.isCollision) {
		// Non-collision records are hints, so resolve the name in the
		// chapter.
		bool found;
		int result = getRecordFromZone(zone, request, &found,
					       record.virtualChapter);
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (!found) {
			// The name does not exist in the chapter, so there is
			// nothing to remove.
			return UDS_SUCCESS;
		}
	}

	request->location = computeIndexRegion(zone, record.virtualChapter);

	/*
	 * Delete the master index entry for the named record only. Note that a
	 * later search might later return stale advice if there is a colliding
	 * name in the same chapter, but it's a very rare case (1 in 2^21).
	 */
	result = removeMasterIndexRecord(&record);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// If the record is in the open chapter, we must remove it or mark it
	// deleted to avoid trouble if the record is added again later.
	if (request->location == LOC_IN_OPEN_CHAPTER) {
		bool hash_exists = false;
		removeFromOpenChapter(zone->openChapter, &request->chunkName,
				      &hash_exists);
		result = ASSERT(hash_exists,
				"removing record not found in open chapter");
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	return UDS_SUCCESS;
}

/**
 * Simulate the creation of a sparse cache barrier message by the triage
 * queue, and the later execution of that message in an index zone.
 *
 * If the index receiving the request is multi-zone or dense, this function
 * does nothing. This simulation is an optimization for single-zone sparse
 * indexes. It also supports unit testing of indexes without routers and
 * queues.
 *
 * @param zone	   the index zone responsible for the index request
 * @param request  the index request about to be executed
 *
 * @return UDS_SUCCESS always
 **/
static int simulate_index_zone_barrier_message(struct index_zone *zone,
					       Request *request)
{
	// Do nothing unless this is a single-zone sparse index.
	if ((zone->index->zone_count > 1) ||
	    !is_sparse(zone->index->volume->geometry)) {
		return UDS_SUCCESS;
	}

	// Check if the index request is for a sampled name in a sparse
	// chapter.
	uint64_t sparse_virtual_chapter =
		triage_index_request(zone->index, request);
	if (sparse_virtual_chapter == UINT64_MAX) {
		// Not indexed, not a hook, or in a chapter that is still
		// dense, which means there should be no change to the sparse
		// chapter index cache.
		return UDS_SUCCESS;
	}

	/*
	 * The triage queue would have generated and enqueued a barrier message
	 * preceding this request, which we simulate by directly invoking the
	 * execution hook for an equivalent message.
	 */
	BarrierMessageData barrier =
		{ .virtualChapter = sparse_virtual_chapter };
	return executeSparseCacheBarrierMessage(zone, &barrier);
}

/**********************************************************************/
static int dispatch_index_zone_request(struct index_zone *zone,
				       Request *request)
{
	if (!request->requeued) {
		// Single-zone sparse indexes don't have a triage queue to
		// generate cache barrier requests, so see if we need to
		// synthesize a barrier.
		int result =
			simulate_index_zone_barrier_message(zone, request);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	// Set the default location. It will be overwritten if we find the
	// chunk.
	request->location = LOC_UNAVAILABLE;

	int result;
	switch (request->action) {
	case REQUEST_INDEX:
	case REQUEST_UPDATE:
	case REQUEST_QUERY:
		result = makeUnrecoverable(search_index_zone(zone, request));
		break;

	case REQUEST_DELETE:
		result =
			makeUnrecoverable(remove_from_index_zone(zone, request));
		break;

	default:
		result = logWarningWithStringError(UDS_INVALID_ARGUMENT,
						   "attempted to execute invalid action: %d",
						   request->action);
		break;
	}

	return result;
}

/**********************************************************************/
int dispatch_index_request(struct index *index, Request *request)
{
	return dispatch_index_zone_request(get_request_zone(index, request),
					   request);
}

/**********************************************************************/
static int rebuild_index_page_map(struct index *index, uint64_t vcn)
{
	struct geometry *geometry = index->volume->geometry;
	unsigned int chapter = map_to_physical_chapter(geometry, vcn);
	unsigned int expected_list_number = 0;
	unsigned int index_page_number;
	for (index_page_number = 0;
	     index_page_number < geometry->index_pages_per_chapter;
	     index_page_number++) {
		struct delta_index_page *chapter_index_page;
		int result = getPage(index->volume,
				     chapter,
				     index_page_number,
				     CACHE_PROBE_INDEX_FIRST,
				     NULL,
				     &chapter_index_page);
		if (result != UDS_SUCCESS) {
			return logErrorWithStringError(result,
						       "failed to read index page %u in chapter %u",
						       index_page_number,
						       chapter);
		}
		unsigned int lowest_delta_list =
			chapter_index_page->lowest_list_number;
		unsigned int highest_delta_list =
			chapter_index_page->highest_list_number;
		if (lowest_delta_list != expected_list_number) {
			return logErrorWithStringError(UDS_CORRUPT_DATA,
						       "chapter %u index page %u is corrupt",
						       chapter,
						       index_page_number);
		}
		result = update_index_page_map(index->volume->indexPageMap,
					       vcn,
					       chapter,
					       index_page_number,
					       highest_delta_list);
		if (result != UDS_SUCCESS) {
			return logErrorWithStringError(result,
						       "failed to update chapter %u index page %u",
						       chapter,
						       index_page_number);
		}
		expected_list_number = highest_delta_list + 1;
	}
	return UDS_SUCCESS;
}

/**
 * Add an entry to the master index when rebuilding.
 *
 * @param index			  The index to query.
 * @param name			  The block name of interest.
 * @param virtual_chapter	  The virtual chapter number to write to the
 *				  master index
 * @param will_be_sparse_chapter  True if this entry will be in the sparse
 *				  portion of the index at the end of
 *				  rebuilding
 *
 * @return UDS_SUCCESS or an error code
 **/
static int replay_record(struct index *index,
			 const struct uds_chunk_name *name,
			 uint64_t virtual_chapter,
			 bool will_be_sparse_chapter)
{
	if (will_be_sparse_chapter &&
	    !isMasterIndexSample(index->master_index, name)) {
		// This entry will be in a sparse chapter after the rebuild
		// completes, and it is not a sample, so just skip over it.
		return UDS_SUCCESS;
	}

	MasterIndexRecord record;
	int result = getMasterIndexRecord(index->master_index, name, &record);
	if (result != UDS_SUCCESS) {
		return result;
	}

	bool update_record;
	if (record.isFound) {
		if (record.isCollision) {
			if (record.virtualChapter == virtual_chapter) {
				/* The record is already correct, so we don't
				 * need to do anything */
				return UDS_SUCCESS;
			}
			update_record = true;
		} else if (record.virtualChapter == virtual_chapter) {
			/*
			 * There is a master index entry pointing to the
			 * current chapter, but we don't know if it is for the
			 * same name as the one we are currently working on or
			 * not. For now, we're just going to assume that it
			 * isn't. This will create one extra collision record
			 * if there was a deleted record in the current
			 * chapter.
			 */
			update_record = false;
		} else {
			/*
			 * If we're rebuilding, we don't normally want to go to
			 * disk to see if the record exists, since we will
			 * likely have just read the record from disk (i.e. we
			 * know it's there). The exception to this is when we
			 * already find an entry in the master index that has a
			 * different chapter. In this case, we need to search
			 * that chapter to determine if the master index entry
			 * was for the same record or a different one.
			 */
			result = searchVolumePageCache(index->volume,
						       NULL, name,
						       record.virtualChapter,
						       NULL, &update_record);
			if (result != UDS_SUCCESS) {
				return result;
			}
		}
	} else {
		update_record = false;
	}

	if (update_record) {
		/*
		 * Update the master index to reference the new chapter for the
		 * block. If the record had been deleted or dropped from the
		 * chapter index, it will be back.
		 */
		result = setMasterIndexRecordChapter(&record, virtual_chapter);
	} else {
		/*
		 * Add a new entry to the master index referencing the open
		 * chapter. This should be done regardless of whether we are a
		 * brand new record or a sparse record, i.e. one that doesn't
		 * exist in the index but does on disk, since for a sparse
		 * record, we would want to un-sparsify if it did exist.
		 */
		result = putMasterIndexRecord(&record, virtual_chapter);
	}

	if ((result == UDS_DUPLICATE_NAME) || (result == UDS_OVERFLOW)) {
		/* Ignore duplicate record and delta list overflow errors */
		return UDS_SUCCESS;
	}

	return result;
}

/**********************************************************************/
void begin_save(struct index *index,
		bool checkpoint,
		uint64_t open_chapter_number)
{
	index->prev_checkpoint = index->last_checkpoint;
	index->last_checkpoint =
		((open_chapter_number == 0) ? NO_LAST_CHECKPOINT :
					      open_chapter_number - 1);

	const char *what = (checkpoint ? "checkpoint" : "save");
	logInfo("beginning %s (vcn %" PRIu64 ")", what,
		index->last_checkpoint);
}

/**
 * Suspend the index if necessary and wait for a signal to resume.
 *
 * @param index	 The index to replay
 *
 * @return <code>true</code> if the replay should terminate
 **/
static bool check_for_suspend(struct index *index)
{
	if (index->load_context == NULL) {
		return false;
	}

	lockMutex(&index->load_context->mutex);
	if (index->load_context->status != INDEX_SUSPENDING) {
		unlockMutex(&index->load_context->mutex);
		return false;
	}

	// Notify that we are suspended and wait for the resume.
	index->load_context->status = INDEX_SUSPENDED;
	broadcastCond(&index->load_context->cond);

	while ((index->load_context->status != INDEX_OPENING) &&
	       (index->load_context->status != INDEX_FREEING)) {
		waitCond(&index->load_context->cond,
			 &index->load_context->mutex);
	}

	bool ret_val = (index->load_context->status == INDEX_FREEING);
	unlockMutex(&index->load_context->mutex);
	return ret_val;
}

/**********************************************************************/
int replay_volume(struct index *index, uint64_t from_vcn)
{
	int result;
	uint64_t upto_vcn = index->newest_virtual_chapter;
	logInfo("Replaying volume from chapter %" PRIu64 " through chapter %" PRIu64,
		from_vcn,
		upto_vcn);
	setMasterIndexOpenChapter(index->master_index, upto_vcn);
	setMasterIndexOpenChapter(index->master_index, from_vcn);

	/*
	 * At least two cases to deal with here!
	 * - index loaded but replaying from last_checkpoint; maybe full, maybe
	 * not
	 * - index failed to load, full rebuild
	 *   Starts empty, then dense-only, then dense-plus-sparse.
	 *   Need to sparsify while processing individual chapters.
	 */
	IndexLookupMode old_lookup_mode = index->volume->lookupMode;
	index->volume->lookupMode = LOOKUP_FOR_REBUILD;
	/*
	 * Go through each record page of each chapter and add the records back
	 * to the master index.	 This should not cause anything to be written
	 * to either the open chapter or on disk volume.  Also skip the on disk
	 * chapter corresponding to upto, as this would have already been
	 * purged from the master index when the chapter was opened.
	 *
	 * Also, go through each index page for each chapter and rebuild the
	 * index page map.
	 */
	const struct geometry *geometry = index->volume->geometry;
	uint64_t old_ipm_update = get_last_update(index->volume->indexPageMap);
	uint64_t vcn;
	for (vcn = from_vcn; vcn < upto_vcn; ++vcn) {
		if (check_for_suspend(index)) {
			logInfo("Replay interrupted by index shutdown at chapter %" PRIu64,
				vcn);
			return UDS_SHUTTINGDOWN;
		}

		bool will_be_sparse_chapter =
			is_chapter_sparse(geometry, from_vcn, upto_vcn, vcn);
		unsigned int chapter = map_to_physical_chapter(geometry, vcn);
		prefetch_volume_pages(&index->volume->volumeStore,
				      mapToPhysicalPage(geometry, chapter, 0),
				      geometry->pages_per_chapter);
		setMasterIndexOpenChapter(index->master_index, vcn);
		result = rebuild_index_page_map(index, vcn);
		if (result != UDS_SUCCESS) {
			index->volume->lookupMode = old_lookup_mode;
			return logErrorWithStringError(result,
						       "could not rebuild index page map for chapter %u",
						       chapter);
		}

		unsigned int j;
		for (j = 0; j < geometry->record_pages_per_chapter; j++) {
			unsigned int record_page_number =
				geometry->index_pages_per_chapter + j;
			byte *record_page;
			result = getPage(index->volume, chapter,
					 record_page_number,
					 CACHE_PROBE_RECORD_FIRST,
					 &record_page, NULL);
			if (result != UDS_SUCCESS) {
				index->volume->lookupMode = old_lookup_mode;
				return logUnrecoverable(result,
							"could not get page %d",
							record_page_number);
			}
			unsigned int k;
			for (k = 0; k < geometry->records_per_page; k++) {
				const byte *name_bytes =
					record_page + (k * BYTES_PER_RECORD);

				struct uds_chunk_name name;
				memcpy(&name.name, name_bytes,
				       UDS_CHUNK_NAME_SIZE);

				result = replay_record(index, &name, vcn,
						       will_be_sparse_chapter);
				if (result != UDS_SUCCESS) {
					char hex_name[(2 * UDS_CHUNK_NAME_SIZE) +
						      1];
					if (chunk_name_to_hex(&name, hex_name,
							      sizeof(hex_name)) !=
					    UDS_SUCCESS) {
						strncpy(hex_name, "<unknown>",
							sizeof(hex_name));
					}
					index->volume->lookupMode =
						old_lookup_mode;
					return logUnrecoverable(result,
								"could not find block %s during rebuild",
								hex_name);
				}
			}
		}
	}
	index->volume->lookupMode = old_lookup_mode;

	// We also need to reap the chapter being replaced by the open chapter
	setMasterIndexOpenChapter(index->master_index, upto_vcn);

	uint64_t new_ipm_update = get_last_update(index->volume->indexPageMap);

	if (new_ipm_update != old_ipm_update) {
		logInfo("replay changed index page map update from %" PRIu64 " to %" PRIu64,
			old_ipm_update,
			new_ipm_update);
	}

	return UDS_SUCCESS;
}

/**********************************************************************/
void get_index_stats(struct index *index, struct uds_index_stats *counters)
{
	uint64_t cw_allocated =
		get_chapter_writer_memory_allocated(index->chapter_writer);
	// We're accessing the master index while not on a zone thread, but
	// that's safe to do when acquiring statistics.
	MasterIndexStats dense_stats, sparse_stats;
	getMasterIndexStats(index->master_index, &dense_stats, &sparse_stats);

	counters->entriesIndexed =
		(dense_stats.recordCount + sparse_stats.recordCount);
	counters->memoryUsed =
		((uint64_t) dense_stats.memoryAllocated +
		 (uint64_t) sparse_stats.memoryAllocated +
		 (uint64_t) getCacheSize(index->volume) + cw_allocated);
	counters->collisions =
		(dense_stats.collisionCount + sparse_stats.collisionCount);
	counters->entriesDiscarded =
		(dense_stats.discardCount + sparse_stats.discardCount);
	counters->checkpoints = get_checkpoint_count(index->checkpoint);
}

/**********************************************************************/
void advance_active_chapters(struct index *index)
{
	index->newest_virtual_chapter++;
	if (are_same_physical_chapter(index->volume->geometry,
				      index->newest_virtual_chapter,
				      index->oldest_virtual_chapter)) {
		index->oldest_virtual_chapter++;
	}
}

/**********************************************************************/
uint64_t triage_index_request(struct index *index, Request *request)
{
	MasterIndexTriage triage;
	lookupMasterIndexName(index->master_index, &request->chunkName,
			      &triage);
	if (!triage.inSampledChapter) {
		// Not indexed or not a hook.
		return UINT64_MAX;
	}

	struct index_zone *zone = get_request_zone(index, request);
	if (!isZoneChapterSparse(zone, triage.virtualChapter)) {
		return UINT64_MAX;
	}

	// XXX Optimize for a common case by remembering the chapter from the
	// most recent barrier message and skipping this chapter if is it the
	// same.

	// Return the sparse chapter number to trigger the barrier messages.
	return triage.virtualChapter;
}
