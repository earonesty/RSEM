#ifndef BAMWRITER_H_
#define BAMWRITER_H_

#include<cmath>
#include<cstdio>
#include<cstring>
#include<cassert>
#include<string>
#include<sstream>

#include <stdint.h>
#include "sam/bam.h"
#include "sam/sam.h"
#include "sam_rsem_aux.h"
#include "sam_rsem_cvt.h"

#include "SingleHit.h"
#include "PairedEndHit.h"

#include "HitWrapper.h"
#include "Transcript.h"
#include "Transcripts.h"

class BamWriter {
public:
	BamWriter(char, const char*, const char*, const char*, Transcripts&);
	~BamWriter();

	void work(HitWrapper<SingleHit>);
	void work(HitWrapper<PairedEndHit>);
private:
	samfile_t *in, *out;
	Transcripts& transcripts;

	//convert bam1_t
	void convert(bam1_t*, double);
};

//fn_list can be NULL
BamWriter::BamWriter(char inpType, const char* inpF, const char* fn_list, const char* outF, Transcripts& transcripts)
	: transcripts(transcripts)
{
	switch(inpType) {
	case 's': in = samopen(inpF, "r", fn_list); break;
	case 'b': in = samopen(inpF, "rb", fn_list); break;
	default: assert(false);
	}
	assert(in != 0);

	//generate output's header
	bam_header_t *out_header = bam_header_dwt(in->header);

	if (out_header->n_targets != transcripts.getM()) {
		fprintf(stderr, "Number of reference sequences recorded in the header is not correct! The header contains %d sequences while there should be %d sequences\n", out_header->n_targets, transcripts.getM());
		exit(-1);
	}

	for (int i = 0; i < out_header->n_targets; i++) {
		const Transcript& transcript = transcripts.getTranscriptAt(i + 1);
		if (out_header->target_name[i] != transcript.getTranscriptID()) {
			fprintf(stderr, "Reference sequence %d's name recorded in the header is not correct! \n", i);
			fprintf(stderr, "Name in the header: %s\n", out_header->target_name[i]);
			fprintf(stderr, "Should be: %s\n", transcript.getTranscriptID().c_str());
			exit(-1);
		}
		out_header->target_len[i] = transcript.getLength();  // transcript length without poly(A) tail
	}

	std::ostringstream strout;
	strout<<"@HD\tVN:1.4\tSO:unknown\n@PG\tID:RSEM\n";
	std::string content = strout.str();
	append_header_text(out_header, content.c_str(), content.length());

	out = samopen(outF, "wb", out_header);
	assert(out != 0);

	bam_header_destroy(out_header);
}

BamWriter::~BamWriter() {
	samclose(in);
	samclose(out);
}

void BamWriter::work(HitWrapper<SingleHit> wrapper) {
	bam1_t *b;
	SingleHit *hit;

	int cnt = 0;

	b = bam_init1();

	while (samread(in, b) >= 0) {
		++cnt;
		if (verbose && cnt % 1000000 == 0) { printf("%d alignment lines are loaded!\n", cnt); }

		if (b->core.flag & 0x0004) continue;

		hit = wrapper.getNextHit();
		assert(hit != NULL);

		assert(b->core.tid + 1 == hit->getSid());
		convert(b, hit->getConPrb());
		if (b->core.qual > 0) samwrite(out, b); // output only when MAPQ > 0
	}

	assert(wrapper.getNextHit() == NULL);

	bam_destroy1(b);
	if (verbose) { printf("Bam output file is generated!\n"); }
}

void BamWriter::work(HitWrapper<PairedEndHit> wrapper) {
	bam1_t *b, *b2;
	PairedEndHit *hit;

	int cnt = 0;

	b = bam_init1();
	b2 = bam_init1();

	while (samread(in, b) >= 0 && samread(in, b2) >= 0) {
		cnt += 2;
		if (verbose && cnt % 1000000 == 0) { printf("%d alignment lines are loaded!\n", cnt); }

		if ((b->core.flag & 0x0004) || (b2->core.flag & 0x0004)) continue;

		//swap if b is mate 2
		if (b->core.flag & 0x0080) {
			assert(b2->core.flag & 0x0040);
			bam1_t *tmp = b;
			b = b2; b2 = tmp;
		}

		hit = wrapper.getNextHit();
		assert(hit != NULL);

		assert(b->core.tid + 1 == hit->getSid());
		assert(b2->core.tid + 1 == hit->getSid());

		convert(b, hit->getConPrb());
		convert(b2, hit->getConPrb());

		b->core.mpos = b2->core.pos;
		b2->core.mpos = b->core.pos;

		if (b->core.qual > 0) {
			samwrite(out, b);
			samwrite(out, b2);
		}
	}

	assert(wrapper.getNextHit() == NULL);

	bam_destroy1(b);
	bam_destroy1(b2);

	if (verbose) { printf("Bam output file is generated!\n"); }
}

void BamWriter::convert(bam1_t *b, double prb) {
	int sid = b->core.tid + 1;
	const Transcript& transcript = transcripts.getTranscriptAt(sid);

	int pos = b->core.pos;
	int readlen = b->core.l_qseq;

	std::vector<uint32_t> data;
	data.clear();

	int core_pos, core_n_cigar;
	std::vector<Interval> vec;
	vec.assign(1, Interval(1, transcript.getLength()));
	// make an artificial chromosome coordinates for the transcript to get new CIGAR strings
	tr2chr(Transcript("", "", "", '+', vec, ""), pos + 1, pos + readlen, core_pos, core_n_cigar, data);
	assert(core_pos >= 0);

	int rest_len = b->data_len - b->core.l_qname - b->core.n_cigar * 4;
	b->data_len = b->core.l_qname + core_n_cigar * 4 + rest_len;
	expand_data_size(b);
	uint8_t* pt = b->data + b->core.l_qname;
	memmove(pt + core_n_cigar * 4, pt + b->core.n_cigar * 4, rest_len);
	for (int i = 0; i < core_n_cigar; i++) { memmove(pt, &data[i], 4); pt += 4; }

	b->core.pos = core_pos;
	b->core.n_cigar = core_n_cigar;
	b->core.qual = getMAPQ(prb);
	b->core.bin = bam_reg2bin(b->core.pos, bam_calend(&(b->core), bam1_cigar(b)));

	float val = (float)prb;
	bam_aux_append(b, "ZW", 'f', bam_aux_type2size('f'), (uint8_t*)&val);
}

#endif /* BAMWRITER_H_ */
