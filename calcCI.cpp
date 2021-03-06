#include<ctime>
#include<cstdio>
#include<cstring>
#include<cstdlib>
#include<cassert>
#include<fstream>
#include<algorithm>
#include<vector>
#include<pthread.h>

#include "utils.h"
#include "my_assert.h"
#include "sampling.h"

#include "Model.h"
#include "SingleModel.h"
#include "SingleQModel.h"
#include "PairedEndModel.h"
#include "PairedEndQModel.h"

#include "Refs.h"
#include "GroupInfo.h"

#include "Buffer.h"
using namespace std;

struct Params {
	int no;
	FILE *fi;
	engine_type *engine;
	double *mw;
};

struct CIType {
	float lb, ub; // the interval is [lb, ub]

	CIType() { lb = ub = 0.0; }
};

struct CIParams {
	int no;
	int start_gene_id, end_gene_id;
};

int model_type;

int nMB;
double confidence;
int nCV, nSpC, nSamples; // nCV: number of count vectors; nSpC: number of theta vectors sampled per count vector; nSamples: nCV * nSpC
int nThreads;
int cvlen;

char cvsF[STRLEN], tmpF[STRLEN], command[STRLEN];

CIType *iso_tau, *gene_tau;

int M, m;
Refs refs;
GroupInfo gi;
char imdName[STRLEN], statName[STRLEN];
char modelF[STRLEN], groupF[STRLEN], refF[STRLEN];

vector<double> eel; //expected effective lengths

Buffer *buffer;

bool quiet;

Params *paramsArray;
pthread_t *threads;
pthread_attr_t attr;
void *status;
int rc;

CIParams *ciParamsArray;

template<class ModelType>
void calcExpectedEffectiveLengths(ModelType& model) {
	int lb, ub, span;
	double *pdf = NULL, *cdf = NULL, *clen = NULL; // clen[i] = \sigma_{j=1}^{i}pdf[i]*(lb+i)
  
	model.getGLD().copyTo(pdf, cdf, lb, ub, span);
	clen = new double[span + 1];
	clen[0] = 0.0;
	for (int i = 1; i <= span; i++) {
		clen[i] = clen[i - 1] + pdf[i] * (lb + i);
	}

	eel.assign(M + 1, 0.0);
	for (int i = 1; i <= M; i++) {
		int totLen = refs.getRef(i).getTotLen();
		int fullLen = refs.getRef(i).getFullLen();
		int pos1 = max(min(totLen - fullLen + 1, ub) - lb, 0);
		int pos2 = max(min(totLen, ub) - lb, 0);

		if (pos2 == 0) { eel[i] = 0.0; continue; }
    
		eel[i] = fullLen * cdf[pos1] + ((cdf[pos2] - cdf[pos1]) * (totLen + 1) - (clen[pos2] - clen[pos1]));
		assert(eel[i] >= 0);
		if (eel[i] < MINEEL) { eel[i] = 0.0; }
	}
  
	delete[] pdf;
	delete[] cdf;
	delete[] clen;
}

void* sample_theta_from_c(void* arg) {

	int *cvec;
	double *theta;
	gamma_dist **gammas;
	gamma_generator **rgs;

	Params *params = (Params*)arg;
	FILE *fi = params->fi;
	double *mw = params->mw;

	cvec = new int[cvlen];
	theta = new double[cvlen];
	gammas = new gamma_dist*[cvlen];
	rgs = new gamma_generator*[cvlen];

	float **vecs = new float*[nSpC];
	for (int i = 0; i < nSpC; i++) vecs[i] = new float[cvlen];

	int cnt = 0;
	while (fscanf(fi, "%d", &cvec[0]) == 1) {
		for (int j = 1; j < cvlen; j++) assert(fscanf(fi, "%d", &cvec[j]) == 1);

		++cnt;

		for (int j = 0; j < cvlen; j++) {
			gammas[j] = new gamma_dist(cvec[j]);
			rgs[j] = new gamma_generator(*(params->engine), *gammas[j]);
		}

		for (int i = 0; i < nSpC; i++) {
			double sum = 0.0;
			for (int j = 0; j < cvlen; j++) {
				theta[j] = ((j == 0 || eel[j] >= EPSILON) ? (*rgs[j])() : 0.0);
				sum += theta[j];
			}
			assert(sum >= EPSILON);
			for (int j = 0; j < cvlen; j++) theta[j] /= sum;

			sum = 0.0;
			for (int j = 0; j < cvlen; j++) {
				theta[j] = (mw[j] < EPSILON ? 0.0 : theta[j] / mw[j]);
				sum += theta[j];
			}
			assert(sum >= EPSILON);
			for (int j = 0; j < cvlen; j++) theta[j] /= sum;


			sum = 0.0;
			vecs[i][0] = theta[0];
			for (int j = 1; j < cvlen; j++)
				if (eel[j] >= EPSILON) {
					vecs[i][j] = theta[j] / eel[j];
					sum += vecs[i][j];
				}
				else assert(theta[j] < EPSILON);

			assert(sum >= EPSILON);
			for (int j = 1; j < cvlen; j++) vecs[i][j] /= sum;
		}

		buffer->write(nSpC, vecs);

		for (int j = 0; j < cvlen; j++) {
			delete gammas[j];
			delete rgs[j];
		}

		if (verbose && cnt % 100 == 0) { printf("Thread %d, %d count vectors are processed!\n", params->no, cnt); }
	}

	delete[] cvec;
	delete[] theta;
	delete[] gammas;
	delete[] rgs;

	for (int i = 0; i < nSpC; i++) delete[] vecs[i];
	delete[] vecs;

	return NULL;
}

template<class ModelType>
void sample_theta_vectors_from_count_vectors() {
	ModelType model;
	model.read(modelF);
	calcExpectedEffectiveLengths<ModelType>(model);

	buffer = new Buffer(nMB, nSamples, cvlen, tmpF);

	paramsArray = new Params[nThreads];
	threads = new pthread_t[nThreads];

	char inpF[STRLEN];
	for (int i = 0; i < nThreads; i++) {
		paramsArray[i].no = i;
		sprintf(inpF, "%s%d", cvsF, i);
		paramsArray[i].fi = fopen(inpF, "r");
		paramsArray[i].engine = engineFactory::new_engine();
		paramsArray[i].mw = model.getMW();
	}

	/* set thread attribute to be joinable */
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	for (int i = 0; i < nThreads; i++) {
		rc = pthread_create(&threads[i], &attr, &sample_theta_from_c, (void*)(&paramsArray[i]));
		pthread_assert(rc, "pthread_create", "Cannot create thread " + itos(i) + " (numbered from 0) in sample_theta_vectors_from_count_vectors!");
	}
	for (int i = 0; i < nThreads; i++) {
		rc = pthread_join(threads[i], &status);
		pthread_assert(rc, "pthread_join", "Cannot join thread " + itos(i) + " (numbered from 0) in sample_theta_vectors_from_count_vectors!");
	}

	/* destroy attribute */
	pthread_attr_destroy(&attr);
	delete[] threads;

	for (int i = 0; i < nThreads; i++) {
		fclose(paramsArray[i].fi);
		delete paramsArray[i].engine;
	}
	delete[] paramsArray;

	delete buffer; // Must delete here, force the content left in the buffer be written into the disk

	if (verbose) { printf("Sampling is finished!\n"); }
}

void calcCI(int nSamples, float *samples, float &lb, float &ub) {
	int p, q; // p pointer for lb, q pointer for ub;
	int newp, newq;
	int threshold = nSamples - (int(confidence * nSamples - 1e-8) + 1);
	int nOutside = 0;

	sort(samples, samples + nSamples);

	p = 0; q = nSamples - 1;
	newq = nSamples - 1;
	do {
		q = newq;
		while (newq > 0 && samples[newq - 1] == samples[newq]) newq--;
		newq--;
	} while (newq >= 0 && nSamples - (newq + 1) <= threshold);

	nOutside = nSamples - (q + 1);

	lb = -1e30; ub = 1e30;
	do {
		if (samples[q] - samples[p] < ub - lb) {
			lb = samples[p];
			ub = samples[q];
		}

		newp = p;
		while (newp < nSamples - 1 && samples[newp] == samples[newp + 1]) newp++;
		newp++;
		if (newp <= threshold) {
			nOutside += newp - p;
			p = newp;
			while (nOutside > threshold && q < nSamples - 1) {
				newq = q + 1;
				while (newq < nSamples - 1 && samples[newq] == samples[newq + 1]) newq++;
				nOutside -= newq - q;
				q = newq;
			}
			assert(nOutside <= threshold);
		}
		else p = newp;
	} while (p <= threshold);
}

void* calcCI_batch(void* arg) {
	float *itsamples, *gtsamples;
	ifstream fin;
	CIParams *ciParams = (CIParams*)arg;

	itsamples = new float[nSamples];
	gtsamples = new float[nSamples];

	fin.open(tmpF, ios::binary);
	streampos pos = streampos(gi.spAt(ciParams->start_gene_id)) * nSamples * FLOATSIZE;
	fin.seekg(pos, ios::beg);

	int cnt = 0;
	for (int i = ciParams->start_gene_id; i < ciParams->end_gene_id; i++) {
		int b = gi.spAt(i), e = gi.spAt(i + 1);
		memset(gtsamples, 0, FLOATSIZE * nSamples);
		for (int j = b; j < e; j++) {
			for (int k = 0; k < nSamples; k++) {
				fin.read((char*)(&itsamples[k]), FLOATSIZE);
				gtsamples[k] += itsamples[k];
			}
			calcCI(nSamples, itsamples, iso_tau[j].lb, iso_tau[j].ub);
		}
		calcCI(nSamples, gtsamples, gene_tau[i].lb, gene_tau[i].ub);

		++cnt;
		if (verbose && cnt % 1000 == 0) { printf("In thread %d, %d genes are processed for CI calculation!\n", ciParams->no, cnt); }
	}

	fin.close();

	delete[] itsamples;
	delete[] gtsamples;

	return NULL;
}

void calculate_credibility_intervals(char* imdName) {
	FILE *fo;
	char outF[STRLEN];

	iso_tau = new CIType[M + 1];
	gene_tau = new CIType[m];

	assert(M > 0);
	int quotient = M / nThreads;
	if (quotient < 1) { nThreads = M; quotient = 1; }
	int cur_gene_id = 0;
	int num_isoforms = 0;

	// A just so so strategy for paralleling
	ciParamsArray = new CIParams[nThreads];
	for (int i = 0; i < nThreads; i++) {
		ciParamsArray[i].no = i;
		ciParamsArray[i].start_gene_id = cur_gene_id;
		num_isoforms = 0;

		while ((m - cur_gene_id > nThreads - i - 1) && (i == nThreads - 1 || num_isoforms < quotient)) {
			num_isoforms += gi.spAt(cur_gene_id + 1) - gi.spAt(cur_gene_id);
			++cur_gene_id;
		}

		ciParamsArray[i].end_gene_id = cur_gene_id;
	}

	threads = new pthread_t[nThreads];

	/* set thread attribute to be joinable */
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	// paralleling
	for (int i = 0; i < nThreads; i++) {
		rc = pthread_create(&threads[i], &attr, &calcCI_batch, (void*)(&ciParamsArray[i]));
		pthread_assert(rc, "pthread_create", "Cannot create thread " + itos(i) + " (numbered from 0) in calculate_credibility_intervals!");
	}
	for (int i = 0; i < nThreads; i++) {
		rc = pthread_join(threads[i], &status);
		pthread_assert(rc, "pthread_join", "Cannot join thread " + itos(i) + " (numbered from 0) in calculate_credibility_intervals!");
	}

	// releasing resources

	/* destroy attribute */
	pthread_attr_destroy(&attr);
	delete[] threads;

	delete[] ciParamsArray;

	//isoform level results
	sprintf(outF, "%s.iso_res", imdName);
	fo = fopen(outF, "a");
	for (int i = 1; i <= M; i++)
		fprintf(fo, "%.6g%c", iso_tau[i].lb, (i < M ? '\t' : '\n'));
	for (int i = 1; i <= M; i++)
		fprintf(fo, "%.6g%c", iso_tau[i].ub, (i < M ? '\t' : '\n'));
	fclose(fo);

	//gene level results
	sprintf(outF, "%s.gene_res", imdName);
	fo = fopen(outF, "a");
	for (int i = 0; i < m; i++)
		fprintf(fo, "%.6g%c", gene_tau[i].lb, (i < m - 1 ? '\t' : '\n'));
	for (int i = 0; i < m; i++)
		fprintf(fo, "%.6g%c", gene_tau[i].ub, (i < m - 1 ? '\t' : '\n'));
	fclose(fo);

	delete[] iso_tau;
	delete[] gene_tau;

	if (verbose) { printf("All credibility intervals are calculated!\n"); }
}

int main(int argc, char* argv[]) {
	if (argc < 8) {
		printf("Usage: rsem-calculate-credibility-intervals reference_name sample_name sampleToken confidence nCV nSpC nMB [-p #Threads] [-q]\n");
		exit(-1);
	}

	confidence = atof(argv[4]);
	nCV = atoi(argv[5]);
	nSpC = atoi(argv[6]);
	nMB = atoi(argv[7]);

	nThreads = 1;
	quiet = false;
	for (int i = 8; i < argc; i++) {
		if (!strcmp(argv[i], "-p")) nThreads = atoi(argv[i + 1]);
		if (!strcmp(argv[i], "-q")) quiet = true;
	}
	verbose = !quiet;

	if (nThreads > nCV) {
		nThreads = nCV;
		printf("Warning: Number of count vectors is less than number of threads! Change the number of threads to %d!\n", nThreads);
	}

	sprintf(refF, "%s.seq", argv[1]);
	refs.loadRefs(refF, 1);
	M = refs.getM();
	sprintf(groupF, "%s.grp", argv[1]);
	gi.load(groupF);
	m = gi.getm();

	nSamples = nCV * nSpC;
	cvlen = M + 1;
	assert(nSamples > 0 && cvlen > 1); // for Buffter.h: (bufsize_type)nSamples

	sprintf(imdName, "%s.temp/%s", argv[2], argv[3]);
	sprintf(statName, "%s.stat/%s", argv[2], argv[3]);
	sprintf(tmpF, "%s.tmp", imdName);
	sprintf(cvsF, "%s.countvectors", imdName);

	sprintf(modelF, "%s.model", statName);
	FILE *fi = fopen(modelF, "r");
	general_assert(fi != NULL, "Cannot open " + cstrtos(modelF) + "!");
	assert(fscanf(fi, "%d", &model_type) == 1);
	fclose(fi);

	// Phase I
	switch(model_type) {
	case 0 : sample_theta_vectors_from_count_vectors<SingleModel>(); break;
	case 1 : sample_theta_vectors_from_count_vectors<SingleQModel>(); break;
	case 2 : sample_theta_vectors_from_count_vectors<PairedEndModel>(); break;
	case 3 : sample_theta_vectors_from_count_vectors<PairedEndQModel>(); break;
	}

	// Phase II
	calculate_credibility_intervals(imdName);

	/*
	sprintf(command, "rm -f %s", tmpF);
	int status = system(command);
	if (status != 0) {
		fprintf(stderr, "Cannot delete %s!\n", tmpF);
		exit(-1);
	}
	*/

	return 0;
}
