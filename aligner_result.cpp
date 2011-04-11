/*
 * aligner_result.cpp
 */

#include <iostream>
#include "reference.h"
#include "aligner_result.h"
#include "read.h"
#include "edit.h"
#include "sstring.h"
#include "ds.h"

using namespace std;

/**
 * Get the decoded nucleotide sequence 
 */
void AlnRes::decodedNucsAndQuals(
	const Read& rd,        // read that led to alignment
	BTDnaString& ns,       // out: decoded nucleotides
	BTString& qs) const    // out: decoded qualities
{
	// Walk along the colors
	bool fw = refcoord_.fw();
	assert(color_);
	const size_t rdlen = rd.length();
	const size_t len = rdlen+1;
	ns.resize(len);
	qs.resize(len);
	ns = rd.patFw;
	qs = rd.qual;
	if(!fw) {
		// Reverse the read to make it upstream-to-downstream.  Recall
		// that it's in colorspace, so no need to complement.
		ns.reverse();
		qs.reverse();
		// Need to flip edits around to make them
		// upstream-to-downstream.
		Edit::invertPoss(const_cast<EList<Edit>& >(ced_), rdlen);
	}
	ns.resize(len); ns.set(4, len-1);
	qs.resize(len);
	
	// Convert 3' and 5' nucleotides to upstream and downstream
	// nucleotides
	int nup = fw ? nuc5p_ : nuc3p_;
	ASSERT_ONLY(int ndn = fw ? nuc3p_ : nuc5p_);
	
	// Note: the nucleotides in the ned and aed lists are already
	// w/r/t to the Watson strand so there's no need to complement
	// them
	int lastn = nup;
	size_t cedidx = 0;
	int c = ns[0], q = qs[0]-33;
	int lastq = 0;
	for(size_t i = 0; i < rdlen; i++) {
		// If it was determined to have been miscalled, get the
		// decoded subject color
		if(cedidx < ced_.size() && ced_[cedidx].pos == i) {
			assert_neq("ACGTN"[c], ced_[cedidx].chr);
			assert_eq ("ACGTN"[c], ced_[cedidx].qchr);
			c = ced_[cedidx].chr;
			c = asc2dnaOrCol[c];
			q = -q;
			cedidx++;
		}
		// Determine next nucleotide by combining the previous
		// nucleotide and the current color
		int n = nuccol2nuc[lastn][c];
		c = ns[i+1];
		ns.set(n, i+1);
		
		int dq = max(q + lastq, 0);
		dq = min(dq, 127);
		lastq = q;
		q = qs[i+1]-33;
		qs.set(dq+33, i);
		lastn = n;
	}
	ns.set(nup, 0);
	int dq = max(lastq, 0)+33;
	qs.set(min(dq, 127) , rdlen);
	assert_eq(ndn, ns[rdlen]);
	assert_eq(cedidx, ced_.size());
	
	if(!fw) {
		// Need to re-flip edits to make them 5'-to-3' again.
		Edit::invertPoss(const_cast<EList<Edit>& >(ced_), rdlen);
	}
}

#ifndef NDEBUG

/**
 * Assuming this AlnRes is an alignment for 'rd', check that the
 * alignment and 'rd' are compatible with the corresponding
 * reference sequence.
 */
bool AlnRes::matchesRef(
	const Read& rd,
	const BitPairReference& ref)
{
	assert(!empty());
	assert(repOk());
	assert(refcoord_.valid());
	bool fw = refcoord_.fw();
	const size_t rdlen = rd.length();
	// Adjust reference string length according to edits
	char *raw_refbuf = new char[extent_ + 16];
	int nsOnLeft = 0;
	if(refcoord_.off() < 0) {
		nsOnLeft = -((int)refcoord_.off());
	}
	int off = ref.getStretch(
		reinterpret_cast<uint32_t*>(raw_refbuf),
		refcoord_.ref(),
		max<TRefOff>(refcoord_.off(), 0),
		extent_);
	assert_leq(off, 16);
	char *refbuf = raw_refbuf + off;
	
	BTDnaString rf;
	BTDnaString rdseq;
	BTString qseq;
	if(rd.color) {
		// Decode the nucleotide sequence from the alignment
		decodedNucsAndQuals(rd, rdseq, qseq);
	} else {
		// Get the nucleotide sequence from the read
		rdseq = rd.patFw;
		if(!fw) rdseq.reverseComp(false);
	}
	if(!fw) {
		// Invert the nucleotide edits so that they go from upstream to
		// downstream on the Watson strand
		Edit::invertPoss(ned_, rdlen + (color() ? 1 : 0));
	}
	// rdseq is the nucleotide sequence (decoded in the case of a
	// colorspace read) from upstream to downstream on the Watson
	// strand.  ned_ are the nucleotide edits from upstream to
	// downstream.  rf contains the reference characters.
	Edit::toRef(rdseq, ned_, rf);
	if(!fw) {
		// Re-invert the nucleotide edits so that they go from 5' to 3'
		Edit::invertPoss(ned_, rdlen + (color() ? 1 : 0));
	}
	assert_eq(extent_ + (color_ ? 1 : 0), rf.length());
	EList<bool> matches;
	bool matchesOverall = true;
	matches.resize(extent_);
	matches.fill(true);
	for(size_t i = 0; i < extent_; i++) {
		if((int)i < nsOnLeft) {
			if((int)rf[i] != 4) {
				matches[i] = false;
				matchesOverall = false;
			}
		} else {
			if((int)rf[i] != (int)refbuf[i-nsOnLeft]) {
				matches[i] = false;
				matchesOverall = false;
			}
		}
	}
	if(!matchesOverall) {
		// Print a friendly message showing the difference between the
		// reference sequence obtained with Edit::toRef and the actual
		// reference sequence
		cerr << endl;
		Edit::printQAlignNoCheck(
			cerr,
			"    ",
			rdseq,
			ned_);
		cerr << "    ";
		for(size_t i = 0; i < extent_; i++) {
			cerr << (matches[i] ? " " : "*");
		}
		cerr << endl;
		Edit::printQAlign(
			cerr,
			"    ",
			rdseq,
			ned_);
		cerr << endl;
	}
	delete[] raw_refbuf;
	return matchesOverall;
}

#endif /*ndef NDEBUG*/

/**
 * Print the sequence for the read that aligned using A, C, G and
 * T.  This will simply print the read sequence (or its reverse
 * complement) unless this is a colorspace read and printColors is
 * false.  In that case, we print the decoded sequence rather than
 * the original ones.
 */
void AlnRes::printSeq(
	const Read& rd,         // read
	const BTDnaString* dns, // already-decoded nucleotides
	bool printColors,       // true -> print colors instead of decoded nucleotides for colorspace alignment
	bool exEnds,            // true -> exclude ends when printing decoded nucleotides
	OutFileBuf& o) const    // output stream to write to
{
	assert(!rd.patFw.empty());
	bool fw = refcoord_.fw();
	assert(!printColors || rd.color);
	if(!rd.color || printColors) {
		// Print nucleotides or colors
		size_t len = rd.patFw.length();
		for(size_t i = 0; i < len; i++) {
			int c;
			if(fw) {
				c = rd.patFw[i];
			} else {
				c = rd.patFw[len-i-1];
				if(c < 4) c = c ^ 3;
			}
			assert_range(0, 4, c);
			o.write("ACGTN"[c]);
		}
	} else {
		// Print decoded nucleotides
		assert(dns != NULL);
		size_t len = dns->length();
		assert_eq(rd.patFw.length(), len - 1);
		size_t st = 0;
		size_t en = len;
		if(exEnds) {
			st++; en--;
		}
		for(size_t i = st; i < en; i++) {
			int c = dns->get(i);
			assert_range(0, 3, c);
			o.write("ACGT"[c]);
		}
	}
}

/**
 * Print the quality string for the read that aligned.  This will
 * simply print the read qualities (or their reverse) unless this
 * is a colorspace read and printColors is false.  In that case,
 * we print the decoded qualities rather than the original ones.
 */
void AlnRes::printQuals(
	const Read& rd,         // read
	const BTString* dqs,    // already-decoded qualities
	bool printColors,       // true -> print colors instead of decoded nucleotides for colorspace alignment
	bool exEnds,            // true -> exclude ends when printing decoded nucleotides
	OutFileBuf& o) const    // output stream to write to
{
	bool fw = refcoord_.fw();
	size_t len = rd.qual.length();
	assert(!printColors || rd.color);
	if(!rd.color || printColors) {
		// Print original qualities from upstream to downstream Watson
		for(size_t i = 0; i < len; i++) {
			int c = (fw ? rd.qual[i] : rd.qual[len-i-1]);
			o.write(c);
		}
	} else {
		assert(dqs != NULL);
		assert_eq(len+1, dqs->length());
		// Print decoded qualities from upstream to downstream Watson
		if(!exEnds) {
			// Print upstream-most quality
			o.write(dqs->get(0));
		}
		for(size_t i = 0; i < len-1; i++) {
			o.write(dqs->get(i+1));
		}
		if(!exEnds) {
			// Print downstream-most quality
			o.write(dqs->get(len));
		}
	}
}

/**
 * Add all of the cells involved in the given alignment to the database.
 */
void RedundantAlns::add(const AlnRes& res) {
	TRefOff left  = res.refoff();
	TRefOff right = left + res.extent();
	if(!res.fw()) {
		const_cast<AlnRes&>(res).invertEdits();
	}
	const EList<Edit>& ned = res.ned();
	size_t nedidx = 0;
	size_t nrow = res.readLength() + (res.color() ? 1 : 0);
	// For each row...
	for(size_t i = 0; i < nrow; i++) {
		size_t diff = 1;  // amount to shift to right for next round
		right = left + 1;
		while(nedidx < ned.size() && ned[nedidx].pos == i) {
			if(ned[nedidx].isDelete()) {
				// Next my_left will be in same column as this round
				diff = 0;
			}
			nedidx++;
		}
		if(i < nrow-1) {
			// See how many inserts there are before the next read
			// character
			size_t nedidx_next = nedidx;
			while(nedidx_next < ned.size() && ned[nedidx_next].pos == i+1)
			{
				if(ned[nedidx_next].isInsert()) {
					right++;
				}
				nedidx_next++;
			}
		}
		for(TRefOff j = left; j < right; j++) {
			// Add to db
			RedundantCell c(res.refid(), res.fw(), j, i);
			ASSERT_ONLY(bool ret =) cells_.insert(c);
			assert(ret);
		}
		left = right + diff - 1;
	}
	if(!res.fw()) {
		const_cast<AlnRes&>(res).invertEdits();
	}
}

/**
 * Return true iff the given alignment has at least one cell that overlaps
 * one of the cells in the database.
 */
bool RedundantAlns::overlap(const AlnRes& res) {
	TRefOff left  = res.refoff();
	TRefOff right = left + res.extent();
	if(!res.fw()) {
		const_cast<AlnRes&>(res).invertEdits();
	}
	const EList<Edit>& ned = res.ned();
	size_t nedidx = 0;
	size_t nrow = res.readLength() + (res.color() ? 1 : 0);
	// For each row...
	bool olap = false;
	for(size_t i = 0; i < nrow; i++) {
		size_t diff = 1;  // amount to shift to right for next round
		right = left + 1;
		while(nedidx < ned.size() && ned[nedidx].pos == i) {
			if(ned[nedidx].isDelete()) {
				// Next my_left will be in same column as this round
				diff = 0;
			}
			nedidx++;
		}
		if(i < nrow-1) {
			// See how many inserts there are before the next read
			// character
			size_t nedidx_next = nedidx;
			while(nedidx_next < ned.size() && ned[nedidx_next].pos == i+1)
			{
				if(ned[nedidx_next].isInsert()) {
					right++;
				}
				nedidx_next++;
			}
		}
		for(TRefOff j = left; j < right; j++) {
			// Add to db
			RedundantCell c(res.refid(), res.fw(), j, i);
			if(cells_.contains(c)) {
				olap = true;
				break;
			}
		}
		if(olap) {
			break;
		}
		left = right + diff - 1;
	}
	if(!res.fw()) {
		const_cast<AlnRes&>(res).invertEdits();
	}
	return olap;
}

/**
 * Given an unpaired read (in either rd1 or rd2) or a read pair
 * (mate 1 in rd1, mate 2 in rd2).
 */
AlnSetSumm::AlnSetSumm(
	const Read* rd1,
	const Read* rd2,
	const EList<AlnRes>* rs1,
	const EList<AlnRes>* rs2)
{
	assert(rd1 != NULL || rd2 != NULL);
	AlnScore best, secbest;
	best.invalidate();
	secbest.invalidate();
	bool paired = (rs1 != NULL && rs2 != NULL);
	bool noResult = (rs1 == NULL && rs2 == NULL);
	size_t sz;
	if(paired) {
		// Paired alignments
		assert_eq(rs1->size(), rs2->size());
		sz = rs1->size();
		assert_gt(sz, 0);
		for(size_t i = 0; i < rs1->size(); i++) {
			AlnScore sc = (*rs1)[i].score() + (*rs2)[i].score();
			if(sc > best) {
				secbest = best;
				best = sc;
				assert(VALID_AL_SCORE(best));
			} else if(sc > secbest) {
				secbest = sc;
				assert(VALID_AL_SCORE(best));
				assert(VALID_AL_SCORE(secbest));
			}
		}
		init(best, secbest, sz-1);
		assert(!empty());
	} else if(!noResult) {
		// Unpaired alignments
		const EList<AlnRes>* rs = (rs1 != NULL ? rs1 : rs2);
		assert(rs != NULL);
		sz = rs->size();
		assert_gt(sz, 0);
		for(size_t i = 0; i < rs->size(); i++) {
			AlnScore sc = (*rs)[i].score();
			if(sc > best) {
				secbest = best;
				best = sc;
				assert(VALID_AL_SCORE(best));
			} else if(sc > secbest) {
				secbest = sc;
				assert(VALID_AL_SCORE(best));
				assert(VALID_AL_SCORE(secbest));
			}
		}
		init(best, secbest, sz-1);
		assert(!empty());
	} else {
		// No result - leave best and secbest as invalid
		init(best, secbest, 0);
		assert(empty());
	}
}

#ifdef ALIGNER_RESULT_MAIN

#include "mem_ids.h"

int main() {
	{
		// On top of each other, same length
		cerr << "Test case 1, simple overlap 1 ... ";
		AlnRes res1;
		res1.init(
			10,
			AlnScore(),
			NULL,
			NULL,
			NULL,
			Coord(0, 0, true),
			false);
		AlnRes res2;
		res2.init(
			10,
			AlnScore(),
			NULL,
			NULL,
			NULL,
			Coord(0, 0, true),
			false);
		assert(res1.overlap(res2));
		
		// Try again, but using the redundant-alignment database
		RedundantAlns ra;
		ra.add(res1);
		assert(ra.overlap(res1));
		assert(ra.overlap(res2));
		
		cerr << "PASSED" << endl;
	}

	{
		// On top of each other, different lengths
		cerr << "Test case 2, simple overlap 2 ... ";
		AlnRes res1;
		res1.init(
			10,
			AlnScore(),
			NULL,
			NULL,
			NULL,
			Coord(0, 0, true),
			false);
		AlnRes res2;
		res2.init(
			11,
			AlnScore(),
			NULL,
			NULL,
			NULL,
			Coord(0, 0, true),
			false);
		assert(res1.overlap(res2));

		// Try again, but using the redundant-alignment database
		RedundantAlns ra;
		ra.add(res1);
		assert(ra.overlap(res1));
		assert(ra.overlap(res2));

		cerr << "PASSED" << endl;
	}

	{
		// Different references
		cerr << "Test case 3, simple overlap 3 ... ";
		AlnRes res1;
		res1.init(
			10,
			AlnScore(),
			NULL,
			NULL,
			NULL,
			Coord(0, 1, true),
			false);
		AlnRes res2;
		res2.init(
			11,
			AlnScore(),
			NULL,
			NULL,
			NULL,
			Coord(0, 0, true),
			false);
		assert(!res1.overlap(res2));

		// Try again, but using the redundant-alignment database
		RedundantAlns ra;
		ra.add(res1);
		assert(ra.overlap(res1));
		assert(!ra.overlap(res2));

		cerr << "PASSED" << endl;
	}

	{
		// Different references
		cerr << "Test case 4, simple overlap 4 ... ";
		AlnRes res1;
		res1.init(
			10,
			AlnScore(),
			NULL,
			NULL,
			NULL,
			Coord(0, 0, true),
			false);
		AlnRes res2;
		res2.init(
			10,
			AlnScore(),
			NULL,
			NULL,
			NULL,
			Coord(1, 0, true),
			false);
		assert(!res1.overlap(res2));

		// Try again, but using the redundant-alignment database
		RedundantAlns ra;
		ra.add(res1);
		assert(ra.overlap(res1));
		assert(!ra.overlap(res2));

		cerr << "PASSED" << endl;
	}

	{
		// Different strands
		cerr << "Test case 5, simple overlap 5 ... ";
		AlnRes res1;
		res1.init(
			10,
			AlnScore(),
			NULL,
			NULL,
			NULL,
			Coord(0, 0, true),
			false);
		AlnRes res2;
		res2.init(
			10,
			AlnScore(),
			NULL,
			NULL,
			NULL,
			Coord(0, 0, false),
			false);
		assert(!res1.overlap(res2));

		// Try again, but using the redundant-alignment database
		RedundantAlns ra;
		ra.add(res1);
		assert(ra.overlap(res1));
		assert(!ra.overlap(res2));

		cerr << "PASSED" << endl;
	}

	{
		// Different strands
		cerr << "Test case 6, simple overlap 6 ... ";
		EList<Edit> ned1(RES_CAT);
		ned1.expand();
		// 1 step to the right in the middle of the alignment
		ned1.back().init(5, 'A', '-', EDIT_TYPE_INS);
		AlnRes res1;
		res1.init(
			10,
			AlnScore(),
			&ned1,
			NULL,
			NULL,
			Coord(0, 5, false),
			false);
		AlnRes res2;
		res2.init(
			10,
			AlnScore(),
			NULL,
			NULL,
			NULL,
			Coord(0, 6, false),
			false);
		assert(res1.overlap(res2));

		// Try again, but using the redundant-alignment database
		RedundantAlns ra;
		ra.add(res1);
		assert(ra.overlap(res1));
		assert(ra.overlap(res2));

		cerr << "PASSED" << endl;
	}

	{
		// Different strands
		cerr << "Test case 7, simple overlap 7 ... ";
		EList<Edit> ned1(RES_CAT);
		// 3 steps to the right in the middle of the alignment
		ned1.push_back(Edit(5, 'A', '-', EDIT_TYPE_INS));
		ned1.push_back(Edit(5, 'C', '-', EDIT_TYPE_INS));
		ned1.push_back(Edit(5, 'G', '-', EDIT_TYPE_INS));
		AlnRes res1;
		res1.init(
			10,
			AlnScore(),
			&ned1,
			NULL,
			NULL,
			Coord(0, 5, false),
			false);
		AlnRes res2;
		res2.init(
			10,
			AlnScore(),
			NULL,
			NULL,
			NULL,
			Coord(0, 6, false),
			false);
		assert(res1.overlap(res2));

		// Try again, but using the redundant-alignment database
		RedundantAlns ra;
		ra.add(res1);
		assert(ra.overlap(res1));
		assert(ra.overlap(res2));

		cerr << "PASSED" << endl;
	}

	{
		// Both with horizontal movements; overlap
		cerr << "Test case 8, simple overlap 8 ... ";
		EList<Edit> ned1(RES_CAT);
		// 2 steps to the right in the middle of the alignment
		ned1.push_back(Edit(5, 'A', '-', EDIT_TYPE_INS));
		ned1.push_back(Edit(5, 'C', '-', EDIT_TYPE_INS));
		AlnRes res1;
		res1.init(
			10,
			AlnScore(),
			&ned1,
			NULL,
			NULL,
			Coord(0, 5, false),
			false);
		EList<Edit> ned2(RES_CAT);
		// 2 steps to the right in the middle of the alignment
		ned2.push_back(Edit(5, 'A', '-', EDIT_TYPE_INS));
		ned2.push_back(Edit(5, 'C', '-', EDIT_TYPE_INS));
		AlnRes res2;
		res2.init(
			10,
			AlnScore(),
			&ned2,
			NULL,
			NULL,
			Coord(0, 6, false),
			false);
		assert(res1.overlap(res2));

		// Try again, but using the redundant-alignment database
		RedundantAlns ra;
		ra.add(res1);
		assert(ra.overlap(res1));
		assert(ra.overlap(res2));

		cerr << "PASSED" << endl;
	}

	{
		// Both with horizontal movements; no overlap
		cerr << "Test case 9, simple overlap 9 ... ";
		EList<Edit> ned1(RES_CAT);
		// 2 steps to the right in the middle of the alignment
		ned1.push_back(Edit(6, 'A', '-', EDIT_TYPE_INS));
		ned1.push_back(Edit(6, 'C', '-', EDIT_TYPE_INS));
		AlnRes res1;
		res1.init(
			10,
			AlnScore(),
			&ned1,
			NULL,
			NULL,
			Coord(0, 5, true),
			false);
		EList<Edit> ned2(RES_CAT);
		// 2 steps to the right in the middle of the alignment
		ned2.push_back(Edit(5, 'A', '-', EDIT_TYPE_INS));
		ned2.push_back(Edit(5, 'C', '-', EDIT_TYPE_INS));
		AlnRes res2;
		res2.init(
			10,
			AlnScore(),
			&ned2,
			NULL,
			NULL,
			Coord(0, 6, true),
			false);
		assert(!res1.overlap(res2));

		// Try again, but using the redundant-alignment database
		RedundantAlns ra;
		ra.add(res1);
		assert(ra.overlap(res1));
		assert(!ra.overlap(res2));

		cerr << "PASSED" << endl;
	}

	{
		// Both with horizontal movements; no overlap.  Reverse strand.
		cerr << "Test case 10, simple overlap 10 ... ";
		EList<Edit> ned1(RES_CAT);
		// 2 steps to the right in the middle of the alignment
		ned1.push_back(Edit(5, 'A', '-', EDIT_TYPE_INS));
		ned1.push_back(Edit(5, 'C', '-', EDIT_TYPE_INS));
		AlnRes res1;
		res1.init(
			10,
			AlnScore(),
			&ned1,
			NULL,
			NULL,
			Coord(0, 5, false),
			false);
		EList<Edit> ned2(RES_CAT);
		// 2 steps to the right in the middle of the alignment
		ned2.push_back(Edit(6, 'A', '-', EDIT_TYPE_INS));
		ned2.push_back(Edit(6, 'C', '-', EDIT_TYPE_INS));
		AlnRes res2;
		res2.init(
			10,
			AlnScore(),
			&ned2,
			NULL,
			NULL,
			Coord(0, 6, false),
			false);
		assert(!res1.overlap(res2));

		// Try again, but using the redundant-alignment database
		RedundantAlns ra;
		ra.add(res1);
		assert(ra.overlap(res1));
		assert(!ra.overlap(res2));

		cerr << "PASSED" << endl;
	}

	{
		// Both with vertical movements; no overlap
		cerr << "Test case 11, simple overlap 11 ... ";
		EList<Edit> ned1(RES_CAT);
		// 2 steps to the right in the middle of the alignment
		ned1.push_back(Edit(5, '-', 'A', EDIT_TYPE_DEL));
		ned1.push_back(Edit(5, '-', 'C', EDIT_TYPE_DEL));
		AlnRes res1;
		res1.init(
			10,
			AlnScore(),
			&ned1,
			NULL,
			NULL,
			Coord(0, 5, true),
			false);
		EList<Edit> ned2(RES_CAT);
		// 2 steps to the right in the middle of the alignment
		ned2.push_back(Edit(6, '-', 'A', EDIT_TYPE_DEL));
		ned2.push_back(Edit(6, '-', 'C', EDIT_TYPE_DEL));
		AlnRes res2;
		res2.init(
			10,
			AlnScore(),
			&ned2,
			NULL,
			NULL,
			Coord(0, 6, true),
			false);
		assert(!res1.overlap(res2));

		// Try again, but using the redundant-alignment database
		RedundantAlns ra;
		ra.add(res1);
		assert(ra.overlap(res1));
		assert(!ra.overlap(res2));

		cerr << "PASSED" << endl;
	}

	{
		// Both with vertical movements; no overlap
		cerr << "Test case 12, simple overlap 12 ... ";
		EList<Edit> ned1(RES_CAT);
		// 2 steps to the right in the middle of the alignment
		ned1.push_back(Edit(5, '-', 'A', EDIT_TYPE_DEL));
		ned1.push_back(Edit(5, '-', 'C', EDIT_TYPE_DEL));
		AlnRes res1;
		res1.init(
			10,
			AlnScore(),
			&ned1,
			NULL,
			NULL,
			Coord(0, 5, true),
			false);
		EList<Edit> ned2(RES_CAT);
		// 2 steps to the right in the middle of the alignment
		ned2.push_back(Edit(5, '-', 'A', EDIT_TYPE_DEL));
		ned2.push_back(Edit(5, '-', 'C', EDIT_TYPE_DEL));
		AlnRes res2;
		res2.init(
			10,
			AlnScore(),
			&ned2,
			NULL,
			NULL,
			Coord(0, 6, true),
			false);
		assert(!res1.overlap(res2));

		// Try again, but using the redundant-alignment database
		RedundantAlns ra;
		ra.add(res1);
		assert(ra.overlap(res1));
		assert(!ra.overlap(res2));

		cerr << "PASSED" << endl;
	}

	{
		// Both with vertical movements; overlap
		cerr << "Test case 13, simple overlap 13 ... ";
		EList<Edit> ned1(RES_CAT);
		// 2 steps to the right in the middle of the alignment
		ned1.push_back(Edit(5, '-', 'A', EDIT_TYPE_DEL));
		ned1.push_back(Edit(5, '-', 'C', EDIT_TYPE_DEL));
		AlnRes res1;
		res1.init(
			10,
			AlnScore(),
			&ned1,
			NULL,
			NULL,
			Coord(0, 5, true),
			false);
		EList<Edit> ned2(RES_CAT);
		// 2 steps to the right in the middle of the alignment
		ned2.push_back(Edit(4, '-', 'A', EDIT_TYPE_DEL));
		ned2.push_back(Edit(4, '-', 'C', EDIT_TYPE_DEL));
		AlnRes res2;
		res2.init(
			10,
			AlnScore(),
			&ned2,
			NULL,
			NULL,
			Coord(0, 6, true),
			false);
		assert(res1.overlap(res2));

		// Try again, but using the redundant-alignment database
		RedundantAlns ra;
		ra.add(res1);
		assert(ra.overlap(res1));
		assert(ra.overlap(res2));

		cerr << "PASSED" << endl;
	}

	{
		// Not even close
		cerr << "Test case 14, simple overlap 14 ... ";
		EList<Edit> ned1(RES_CAT);
		// 2 steps to the right in the middle of the alignment
		ned1.push_back(Edit(5, '-', 'A', EDIT_TYPE_DEL));
		ned1.push_back(Edit(5, '-', 'C', EDIT_TYPE_DEL));
		AlnRes res1;
		res1.init(
			10,
			AlnScore(),
			&ned1,
			NULL,
			NULL,
			Coord(0, 5, true),
			false);
		EList<Edit> ned2(RES_CAT);
		// 2 steps to the right in the middle of the alignment
		ned2.push_back(Edit(4, '-', 'A', EDIT_TYPE_DEL));
		ned2.push_back(Edit(4, '-', 'C', EDIT_TYPE_DEL));
		AlnRes res2;
		res2.init(
			10,
			AlnScore(),
			&ned2,
			NULL,
			NULL,
			Coord(0, 400, true),
			false);
		assert(!res1.overlap(res2));

		// Try again, but using the redundant-alignment database
		RedundantAlns ra;
		ra.add(res1);
		assert(ra.overlap(res1));
		assert(!ra.overlap(res2));

		cerr << "PASSED" << endl;
	}
}

#endif /*def ALIGNER_RESULT_MAIN*/
