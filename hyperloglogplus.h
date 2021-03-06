/*
 * hyperloglogplus.h
 *
 * Implementation of HyperLogLog++ algorithm described by Stefan Heule et al.
 *
 *  Created on: Apr 25, 2015
 *      Author: fbreitwieser
 */

#ifndef HYPERLOGLOGPLUS_H_
#define HYPERLOGLOGPLUS_H_

#include<set>
#include<vector>
#include<stdexcept>
#include<iostream>
#include<fstream>
#include<math.h>   //log
#include<bitset>

#include "hyperloglogbias.h"
#include "third_party/MurmurHash3.cpp"
using namespace std;

//#define NDEBUG
//#define NDEBUG2
#define arr_len(a) (a + sizeof a / sizeof a[0])

// experimentally determined threshold values for  p - 4
static const uint32_t threshold[] = {10, 20, 40, 80, 220, 400, 900, 1800, 3100,
							  6500, 11500, 20000, 50000, 120000, 350000};


///////////////////////

//
/**
 * gives the estimated cardinality for m bins, v of which are non-zero
 * @param m
 * @param v
 * @return
 */
double linearCounting(uint32_t m, uint32_t v) {
	if (v > m) {
	    throw std::invalid_argument("number of v should not be greater than m");
	}
	double fm = double(m);
	return fm * log(fm/double(v));
}

/**
  * from Numerical Recipes, 3rd Edition, p 352
  * Returns hash of u as a 64-bit integer.
  *
*/
inline uint64_t ranhash (uint64_t u) {
  uint64_t v = u * 3935559000370003845 + 2691343689449507681;

  v ^= v >> 21; v ^= v << 37; v ^= v >>  4;

  v *= 4768777513237032717;

  v ^= v << 20; v ^= v >> 41; v ^= v <<  5;

  return v;
}

inline uint64_t murmurhash3_finalizer (uint64_t key)  {
	key ^= key >> 33;
	key *= 0xff51afd7ed558ccd;
	key ^= key >> 33;
	key *= 0xc4ceb9fe1a85ec53;
	key ^= key >> 33;
	return key;
}

/**
 * Bias correction factors for specific m's
 * @param m
 * @return
 */
double getAlpha(uint32_t m)  {
	switch (m) {
	case 16: return 0.673;
	case 32: return 0.697;
	case 64: return 0.709;
	}

	// m >= 128
	return 0.7213 / (1 + 1.079/double(m));
}

/**
 * calculate the raw estimate as harmonic mean of the ranks in the register
 * @param s
 * @return
 */
double calculateEstimate(vector<uint8_t> M, uint32_t m) {
	double sum = 0.0;
	for (size_t i = 0; i <= M.size(); ++i) {
		sum += 1.0 / (double(uint32_t(1)<<M[i]));
	}

	double dm = double(M.size());
	return getAlpha(m) * dm * dm / sum;
}

uint32_t countZeros(vector<uint8_t> s) {
	return count(s.begin(), s.end(), 0);
}

/**
 * Extract bits (from uint32_t or uint64_t) using LSB 0 numbering from hi to lo, including lo
 * @param bits
 * @param hi
 * @param lo
 * @return
 */
template<typename T>
T extractBits(T bits, uint8_t hi, uint8_t lo) {

    // create a bitmask:
    //            (T(1) << (hi - lo)                 a 1 at the position (hi - lo)
    //           ((T(1) << (hi - lo) - 1)              1's from position 0 to position (hi-lo-1)
    //          (((T(1) << (hi - lo)) - 1) << lo)      1's from position lo to position hi

	// The T(1) is required to not cause overflow on 32bit machines
	// TODO: consider creating a bitmask only once in the beginning
	T bitmask = (((T(1) << (hi - lo)) - 1) << lo);
	
	return ((bits & bitmask) >> lo);
}

template<typename T>
T extractBits(T bits, uint8_t hi) {
    // create a bitmask for first hi bits (LSB 0 numbering)
	T bitmask = (((T(1) << hi) - 1));

	return (bits & bitmask);
}

// functions for counting the number of leading 0-bits (clz)
//           and counting the number of trailing 0-bits (ctz)
//#ifdef __GNUC__

// TODO: switch between builtin clz and 64_clz based on architecture
//#define clz(x) __builtin_clz(x)
#if 0
static int clz_manual(uint64_t x)
{
  // This uses a binary search (counting down) algorithm from Hacker's Delight.
   uint64_t y;
   int n = 64;
   y = x >>32;  if (y != 0) {n -= 32;  x = y;}
   y = x >>16;  if (y != 0) {n -= 16;  x = y;}
   y = x >> 8;  if (y != 0) {n -=  8;  x = y;}
   y = x >> 4;  if (y != 0) {n -=  4;  x = y;}
   y = x >> 2;  if (y != 0) {n -=  2;  x = y;}
   y = x >> 1;  if (y != 0) return n - 2;
   return n - x;
}
#endif

uint32_t clz(const uint32_t x) {
	return __builtin_clz(x);
}

uint32_t clz(const uint64_t x) {
    uint32_t u32 = (x >> 32);
    uint32_t result = u32 ? __builtin_clz(u32) : 32;
    if (result == 32) {
        u32 = x & 0xFFFFFFFFUL;
        result += (u32 ? __builtin_clz(u32) : 32);
    }
    return result;
}
//#else

uint32_t clz_log2(const uint64_t w) {
	return 63 - floor(log2(w));
}
//#endif


// TODO: the sparse list may be encoded with variable length encoding
//   see Heule et al., section 5.3.2
// Also, using sets might give a larger overhead as each insertion costs more
//  consider using vector and sort/unique when merging.
typedef set<uint32_t> SparseListType;
typedef uint64_t HashSize;

/**
 * HyperLogLogPlusMinus class
 * typename T corresponds to the hash size - usually either uint32_t or uint64_t (implemented for uint64_t)
 */

typedef uint64_t T_KEY;
template <typename T_KEY>
class HyperLogLogPlusMinus {

private:

	vector<uint8_t> M;  // registers (M) of size m
	uint8_t p;            // precision
	uint32_t m;           // number of registers
	bool sparse;          // sparse representation of the data?
	SparseListType sparseList; // TODO: use a compressed list instead

	// vectors containing data for bias correction
	vector<vector<double> > rawEstimateData; // TODO: make this static
	vector<vector<double> > biasData;

	// sparse versions of p and m
	static const uint8_t  pPrime = 25; // precision when using a sparse representation
	static const uint32_t mPrime = 1 << (pPrime -1); // 2^pPrime


public:

	~HyperLogLogPlusMinus() {};

	/**
	 * Create new HyperLogLogPlusMinus counter
	 * @param precision
	 * @param sparse
	 */
	HyperLogLogPlusMinus(uint8_t precision=10, bool sparse=true):p(precision),sparse(sparse) {
		if (precision > 18 || precision < 4) {
	        throw std::invalid_argument("precision (number of register = 2^precision) must be between 4 and 18");
		}

		this->m = 1 << precision;

		if (sparse) {
			this->sparseList = SparseListType(); // TODO: if SparseListType is changed, initialize with appropriate size
		} else {
			this->M = vector<uint8_t>(m);
		}
	}

	/**
	 * Aggregation of items. Adds a new item to the counter.
	 * @param item
	 */
	void add(T_KEY item) {
		add(item, sizeof(T_KEY));
	}

	/**
	 * Aggregation of items. Adds a new item to the counter.
	 * @param item
	 * @param size  size of item
	 */
	void add(T_KEY item, size_t size) {

		// compute hash for item
		HashSize x = murmurhash3_finalizer(item);
	
		if (sparse) {
			// sparse mode: put the encoded hash into sparse list
			this->sparseList.insert(encodeHash(x));

			// if the sparseList is too large, switch to normal (register) representation
			if (this->sparseList.size() > this->m) { // TODO: is the size of m correct?
				switchToNormalRepresentation();
			}
		} else {
			// normal mode
			uint32_t idx =    (uint32_t)extractBits(x, 64, 64 - this->p); // get index: {x63,...,x64-p}
			uint8_t rank = (uint8_t)clz(extractBits(x, 64 - this->p, 1)) - this->p;  // get rank of w:  {x63-p,...,x0}

			// update the register if current rank is bigger
			if (rank > this->M[idx]) {
				this->M[idx] = rank;
			}
		}
	}


	void add(vector<T_KEY> words) {
		for(size_t i = 0; i < words.size(); ++i) {
			this->add(words[i]);
		}
	}

	/**
	 * Reset to its initial state.
	 */
	void reset() {
		this->sparse = true;
		this->sparseList.clear();  // 
		this->M.clear();
	}

	/**
	 * Convert from sparse representation (using tmpSet and sparseList) to normal (using register)
	 */
	void switchToNormalRepresentation() {
		this->sparse = false;
		this->M = vector<uint8_t>(this->m);
		if (sparseList.size() > 0) { //TDOD: why do I need to ask this, here?
			addToRegisters(this->sparseList);
			this->sparseList.clear();
		}
	}

	/**
	 * add sparseList to the registers of M
	 */
	void addToRegisters(const SparseListType &sparseList) {
		if (sparseList.size() == 0) {
			return;
		}
		for (SparseListType::const_iterator it = sparseList.begin(); it != sparseList.end(); ++it) {
			idx_n_rank ir = decodeHash(*it);
			assert_lt(ir.idx,M.size());
			if (this->M[ir.idx] < ir.rank) {
				this->M[ir.idx] = ir.rank;
			}
		}
	}

	/**
	 * Merge another HyperLogLogPlusMinus into this. Converts to normal representation
	 * @param other
	 */
	void merge(const HyperLogLogPlusMinus* other) {
		if (this->p != other->p) {
			throw std::invalid_argument("precisions must be equal");
		}

		if (this->sparse && other->sparse) {
			if (this->sparseList.size()+other->sparseList.size() > this->m) {
				switchToNormalRepresentation();
				addToRegisters(other->sparseList);
			} else {
				this->sparseList.insert(other->sparseList.begin(),other->sparseList.end());
			}
		} else if (other->sparse) {
			// other is sparse, but this is not
			addToRegisters(other->sparseList);
		} else {
			if (this->sparse) {
				switchToNormalRepresentation();
			}

			// merge registers
			for (size_t i = 0; i < other->M.size(); ++i) {
				if (other->M[i] > this->M[i]) {
					this->M[i] = other->M[i];
				}
			}
		}
	}

	/**
	 *
	 * @return cardinality estimate
	 */
	uint64_t cardinality() {
		if (sparse) {
			// if we are still 'sparse', then use linear counting, which is more
			//  accurate for low cardinalities, and use increased precision pPrime
			return uint64_t(linearCounting(mPrime, mPrime-uint32_t(sparseList.size())));
		}

		// initialize bias correction data
		if (rawEstimateData.empty()) { initRawEstimateData(); }
		if (biasData.empty())        { initBiasData(); }

		// calculate raw estimate on registers
		double est = calculateEstimate(M, m);

		// correct for biases if estimate is smaller than 5m
		if (est <= double(m)*5.0) {
			est -= getEstimateBias(est);
		}

		uint32_t v = countZeros(M);
		if (v != 0) {
			// calculate linear counting (lc) estimate if there are zeros in the matrix
			double lc_estimate = linearCounting(m, v);

			// check if the lc estimate is below the threshold
			if (lc_estimate <= double(threshold[p-4])) {
				assert_geq(lc_estimate,0);
				if (lc_estimate < 0) {
					throw;
				}
				// return lc estimate of cardinality
				return lc_estimate;
			}
		}

		// return bias-corrected hyperloglog estimate of cardinality
		return uint64_t(est);
	}

private:

    uint8_t rank(HashSize x, uint8_t b) {
        uint8_t v = 1;
        while (v <= b && !(x & 0x80000000)) {
            v++;
            x <<= 1;
        }
        return v;
    }


	void initRawEstimateData() {
	    rawEstimateData = vector<vector<double> >();

	    rawEstimateData.push_back(vector<double>(rawEstimateData_precision4,arr_len(rawEstimateData_precision4)));
	    rawEstimateData.push_back(vector<double>(rawEstimateData_precision5,arr_len(rawEstimateData_precision5)));
	    rawEstimateData.push_back(vector<double>(rawEstimateData_precision6,arr_len(rawEstimateData_precision6)));
	    rawEstimateData.push_back(vector<double>(rawEstimateData_precision7,arr_len(rawEstimateData_precision7)));
	    rawEstimateData.push_back(vector<double>(rawEstimateData_precision8,arr_len(rawEstimateData_precision8)));
	    rawEstimateData.push_back(vector<double>(rawEstimateData_precision9,arr_len(rawEstimateData_precision9)));
	    rawEstimateData.push_back(vector<double>(rawEstimateData_precision10,arr_len(rawEstimateData_precision10)));
	    rawEstimateData.push_back(vector<double>(rawEstimateData_precision11,arr_len(rawEstimateData_precision11)));
	    rawEstimateData.push_back(vector<double>(rawEstimateData_precision12,arr_len(rawEstimateData_precision12)));
	    rawEstimateData.push_back(vector<double>(rawEstimateData_precision13,arr_len(rawEstimateData_precision13)));
	    rawEstimateData.push_back(vector<double>(rawEstimateData_precision14,arr_len(rawEstimateData_precision14)));
	    rawEstimateData.push_back(vector<double>(rawEstimateData_precision15,arr_len(rawEstimateData_precision15)));
	    rawEstimateData.push_back(vector<double>(rawEstimateData_precision16,arr_len(rawEstimateData_precision16)));
	    rawEstimateData.push_back(vector<double>(rawEstimateData_precision17,arr_len(rawEstimateData_precision17)));
	    rawEstimateData.push_back(vector<double>(rawEstimateData_precision18,arr_len(rawEstimateData_precision18)));

	}

	void initBiasData() {
		biasData = vector<vector<double> >();

		biasData.push_back(vector<double>(biasData_precision4,arr_len(biasData_precision4)));
		biasData.push_back(vector<double>(biasData_precision5,arr_len(biasData_precision5)));
		biasData.push_back(vector<double>(biasData_precision6,arr_len(biasData_precision6)));
		biasData.push_back(vector<double>(biasData_precision7,arr_len(biasData_precision7)));
		biasData.push_back(vector<double>(biasData_precision8,arr_len(biasData_precision8)));
		biasData.push_back(vector<double>(biasData_precision9,arr_len(biasData_precision9)));
		biasData.push_back(vector<double>(biasData_precision10,arr_len(biasData_precision10)));
		biasData.push_back(vector<double>(biasData_precision11,arr_len(biasData_precision11)));
		biasData.push_back(vector<double>(biasData_precision12,arr_len(biasData_precision12)));
		biasData.push_back(vector<double>(biasData_precision13,arr_len(biasData_precision13)));
		biasData.push_back(vector<double>(biasData_precision14,arr_len(biasData_precision14)));
		biasData.push_back(vector<double>(biasData_precision15,arr_len(biasData_precision15)));
		biasData.push_back(vector<double>(biasData_precision16,arr_len(biasData_precision16)));
		biasData.push_back(vector<double>(biasData_precision17,arr_len(biasData_precision17)));
		biasData.push_back(vector<double>(biasData_precision18,arr_len(biasData_precision18)));
	}

	/**
	 * Estimate the bias using empirically determined values.
	 * Uses weighted average of the two cells between which the estimate falls.
	 * TODO: Check if nearest neighbor average gives better values, as proposed in the paper
	 * @param est
	 * @return correction value for
	 */
	double getEstimateBias(double estimate) {
		vector<double> rawEstimateTable = rawEstimateData[p-4];
		vector<double> biasTable = biasData[p-4];
	
		// check if estimate is lower than first entry, or larger than last
		if (rawEstimateTable.front() >= estimate) { return rawEstimateTable.front() - biasTable.front(); }
		if (rawEstimateTable.back()  <= estimate) { return rawEstimateTable.back() - biasTable.back(); }
	
		// get iterator to first element that is not smaller than estimate
		vector<double>::const_iterator it = lower_bound(rawEstimateTable.begin(),rawEstimateTable.end(),estimate);
		size_t pos = it - rawEstimateTable.begin();

		double e1 = rawEstimateTable[pos-1];
		double e2 = rawEstimateTable[pos];
	
		double c = (estimate - e1) / (e2 - e1);

		return biasTable[pos-1]*(1-c) + biasTable[pos]*c;
	}
	

	/**
	 * Encode the hash code x as an integer, to be used in the sparse representation.
	 * see section 5.3 in Heule et al.
	 * TODO: I do not understand yet why we can use the 32bit representation here
	 * @param x the hash bits
	 * @return encoded hash value
	 */
	uint32_t encodeHash(uint64_t x) {
		// get the index (the first pPrime bits)
		uint32_t idx = (uint32_t)extractBits(x,64,64-pPrime);

		// maybe replace with: extractBits(idx,this->p-this->pPrime);
		// are the bits {63-p, ..., 63-p'} all 0?
		if (extractBits(x, 64-this->p, 64-pPrime) == 0) {
			// save the rank (and set the last bit to 1)
			uint8_t rank = clz(extractBits(x, 64-pPrime));
			return idx<<7 | uint32_t(rank<<1) | 1;
		} else {
			// else, return the idx, only (left-shifted, last bit = 0)
			return idx << 1;
		}
	}


	/**
	 * struct holding the index and rank/rho of an entry
	 */
	struct idx_n_rank {
		uint32_t idx;
		uint8_t rank;
		idx_n_rank(uint32_t _idx, uint8_t _rank) : idx(_idx), rank(_rank) {}
	};

	//
	//
	/**
	 * Decode a hash from the sparse representation.
	 * Returns the index and number of leading zeros (nlz) with precision p stored in k
	 * @param k the hash bits
	 * @return index and rank in non-sparse format
	 */
	idx_n_rank decodeHash(uint32_t k) const  {

		// check if the last bit is 1
		if ( (k & 1) == 1) {
			// if yes: the hash was stored with higher precision, bits p to pPrime were 0
			uint8_t pp = p + pPrime;
			return(idx_n_rank((uint32_t)extractBits(k, 32, 32 - this->p), // first p bits are the index
							  (uint8_t) extractBits(k, 7, 1) + pp         // bits 7:1 are the rank (minus pp)
			));
		} else {
			// if no: just the pPrime long index was stored, from which both the
			//   p bits long index, as well as the rank can be restored
			return(idx_n_rank((uint32_t)extractBits(k, this->p + 1, 1), //
							  (uint8_t) clz(extractBits(k,this->pPrime - this->p, 1))
			));
		}
	}

	
};




#endif /* HYPERLOGLOGPLUS_H_ */
