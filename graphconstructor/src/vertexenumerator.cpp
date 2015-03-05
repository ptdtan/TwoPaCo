#include <deque>
#include <ctime>
#include <memory>
#include <cassert>
#include <iostream>
#include <algorithm>
#include <unordered_set>
#include <omp.h>
#include "lib/SpookyV2.h"
#include "vertexenumerator.h"

#include "ngramhashing/cyclichash.h"

namespace Sibelia
{	
	const size_t VertexEnumerator::INVALID_VERTEX = -1;

	namespace
	{
		void PutInBloomFilter(std::vector<bool> & bitVector, const std::vector<uint64_t> & seed, const DnaString & item)
		{
			for (const uint64_t & s : seed)
			{
				uint64_t body = item.GetBody();
				uint64_t hvalue = SpookyHash::Hash64(&body, sizeof(body), s);
				bitVector[hvalue % bitVector.size()] = true;
			}
		}

		bool IsInBloomFilter(const std::vector<bool> & bitVector, const std::vector<uint64_t> & seed, const DnaString & item)
		{
			for (const uint64_t & s : seed)
			{
				uint64_t body = item.GetBody();
				uint64_t hvalue = SpookyHash::Hash64(&body, sizeof(body), s);
				if (!bitVector[hvalue % bitVector.size()])
				{
					return false;
				}
			}

			return true;
		}

		class VertexHashFunction
		{
		public:
			VertexHashFunction(size_t vertexSize) : vertexSize_(vertexSize)
			{

			}

			uint64_t operator () (const uint64_t & a) const
			{
				DnaString str(vertexSize_, a);
				uint64_t body = str.GetBody();
				uint64_t hash = SpookyHash::Hash64(&body, sizeof(body), 0);
				return hash;
			}
		private:
			size_t vertexSize_;
		};

		class VertexEquality
		{
		public:
			VertexEquality(size_t vertexSize) : vertexSize_(vertexSize)
			{
				
			}

			bool operator () (const uint64_t & a, const uint64_t & b) const
			{
				DnaString stra(vertexSize_, a);
				DnaString strb(vertexSize_, b);
 				return stra == strb;
			}
		private:
			size_t vertexSize_;
		};

		typedef std::unordered_set<uint64_t, VertexHashFunction, VertexEquality> BifCandidateSet;

		size_t CharIndex(char ch)
		{
			return std::find(DnaString::LITERAL.begin(), DnaString::LITERAL.end(), ch) - DnaString::LITERAL.begin();
		}
	}

	VertexEnumerator::VertexEnumerator(const std::vector<std::string> & fileName, size_t vertexLength, size_t filterSize, size_t q) :
		vertexSize_(vertexLength)
	{
		std::cout << "Filter size = " << filterSize << std::endl;
		if (vertexLength > 30)
		{
			throw std::runtime_error("The vertex size is too large");
		}
		
		std::vector<uint64_t> seed(q);
		std::vector<bool> bitVector(filterSize, false);
		std::generate(seed.begin(), seed.end(), rand);
		size_t edgeLength = vertexLength + 1;
		size_t start = clock();
		for (const std::string & nowFileName : fileName)
		{
			for (StreamFastaParser parser(nowFileName); parser.ReadRecord();)
			{
				char ch;
				DnaString posEdge;
				for (size_t j = 0; j < edgeLength && parser.GetChar(ch); j++)
				{
					posEdge.AppendBack(ch);
				}

				if (posEdge.GetSize() == edgeLength)
				{
					DnaString negEdge = posEdge.RevComp();
					while (true)
					{
						PutInBloomFilter(bitVector, seed, posEdge);
						if (parser.GetChar(ch))
						{
							posEdge.PopFront();
							posEdge.AppendBack(ch);
						}
						else
						{
							break;
						}
					}
				}
			}
		}



		std::cout << "Spooky: " << double(clock() - start) / CLOCKS_PER_SEC << std::endl;
		bitVector.assign(filterSize, false);		

		start = clock();
		for (const std::string & nowFileName : fileName)
		{
			for (StreamFastaParser parser(nowFileName); parser.ReadRecord();)
			{
				char ch;
				DnaString posEdge;
				std::vector<std::unique_ptr<CyclicHash<uint64> > > hf(q);
				for (std::unique_ptr<CyclicHash<uint64> > & ptr : hf)
				{
					ptr.reset(new CyclicHash<uint64>(edgeLength, 64));
				}

				std::deque<char> str;
				for (size_t j = 0; j < edgeLength && parser.GetChar(ch); j++)
				{
					str.push_back(ch);
					for (std::unique_ptr<CyclicHash<uint64> > & ptr : hf)
					{						
						ptr->eat(ch);
					}
				}

				if (str.size() == edgeLength)
				{
					while (true)
					{
						for (std::unique_ptr<CyclicHash<uint64> > & ptr : hf)
						{
							bitVector[ptr->hashvalue % bitVector.size()] = true;
						}

						if (parser.GetChar(ch))
						{
							str.push_back(ch);
							for (std::unique_ptr<CyclicHash<uint64> > & ptr : hf)
							{
								ptr->update(str.front(), ch);
							}
							
							str.pop_front();
						}
						else
						{
							break;
						}
					}
				}
			}
		}
		
		std::cout << "Ngram: " << double(clock() - start) / CLOCKS_PER_SEC << std::endl;

		/*
		std::vector<uint64_t> seed(q);
		std::generate(seed.begin(), seed.end(), rand);		
		size_t edgeLength = vertexLength + 1;
		std::vector<bool> bitVector(filterSize, false);
		std::cout << "Bloom filter counting..." << std::endl;
		omp_set_num_threads(2);
		uint64_t low = 0;
		const size_t MAX_ROUNDS = 3;
		for (size_t round = 0; round < MAX_ROUNDS; round++)
		{
			uint64_t high = round == MAX_ROUNDS - 1 ? UINT64_MAX : (UINT64_MAX / MAX_ROUNDS) * (round + 1);
			for (const std::string & nowFileName : fileName)
			{
				for (StreamFastaParser parser(nowFileName); parser.ReadRecord();)
				{
					char ch;
					DnaString posEdge;
					for (size_t j = 0; j < edgeLength && parser.GetChar(ch); j++)
					{
						posEdge.AppendBack(ch);
					}

					if (posEdge.GetSize() == edgeLength)
					{
						DnaString negEdge = posEdge.RevComp();
						while (true)
						{
							size_t k = 0;
							size_t hit = 0;
							DnaString kmer[2][2] = { { posEdge, negEdge }, { posEdge, negEdge } };							
							for (size_t i = 0; i < 2; i++, k++)
							{								
								kmer[i][k].PopBack();
								kmer[i][1 - k].PopFront();
								uint64_t hvalue = UINT64_MAX;
								assert(kmer[i][0] == kmer[i][1].RevComp());								
								for (size_t j = 0; j < 2; j++)
								{
									uint64_t body = kmer[i][j].GetBody();
									hvalue = std::min(hvalue, SpookyHash::Hash64(&body, sizeof(body), seed[0]));
								}
								
								hit += (hvalue >= low && hvalue <= high) ? 1 : 0;
							}
							
							if (hit)
							{
								PutInBloomFilter(bitVector, seed, posEdge);
							}

							if (parser.GetChar(ch))
							{
								posEdge.PopFront();
								posEdge.AppendBack(ch);
								negEdge.PopBack();
								negEdge.AppendFront(DnaString::Reverse(ch));
								assert(posEdge.RevComp() == negEdge);
							}
							else
							{
								break;
							}
						}
					}
				}
			}

			size_t mark = clock();
			std::cout << "Vertex enumeration..." << std::endl;

			std::unordered_set<uint64_t, VertexHashFunction, VertexEquality> trueBifSet(0, VertexHashFunction(vertexLength), VertexEquality(vertexLength));
			std::unordered_set<uint64_t, VertexHashFunction, VertexEquality> candidateBifSet(0, VertexHashFunction(vertexLength), VertexEquality(vertexLength));
			for (const std::string & nowFileName : fileName)
			{
				for (StreamFastaParser parser(nowFileName); parser.ReadRecord();)
				{
					char posExtend;
					DnaString posVertex;
					for (size_t j = 0; j < vertexLength && parser.GetChar(posExtend); j++)
					{
						posVertex.AppendBack(posExtend);
					}

					if (posVertex.GetSize() >= vertexLength)
					{
						char posPrev;
						char negExtend;
						DnaString negVertex = posVertex.RevComp();
						uint64_t body = posVertex.GetBody();
						uint64_t hvalue = SpookyHash::Hash64(&body, sizeof(body), seed[0]);
						if (hvalue >= low && hvalue <= high)
						{
							trueBifSet.insert(posVertex.GetBody());
						}

						for (bool start = true;; start = false)
						{							
							if (parser.GetChar(posExtend))
							{
								size_t hit = 0;
								uint64_t hvalue = UINT64_MAX;
								DnaString kmer[] = { posVertex, negVertex };
								for (size_t i = 0; i < 2; i++)
								{
									uint64_t body = kmer[i].GetBody();
									hvalue = std::min(hvalue, SpookyHash::Hash64(&body, sizeof(body), seed[0]));									
								}

								if (hvalue >= low && hvalue <= high)
								{
									if (trueBifSet.count(posVertex.GetBody()) == 0 && trueBifSet.count(negVertex.GetBody()) == 0)
									{
										bool posFound = candidateBifSet.count(posVertex.GetBody()) > 0;
										bool negFound = candidateBifSet.count(negVertex.GetBody()) > 0;
										if (!posFound && !negFound)
										{
											size_t inCount = 0;
											size_t outCount = 0;
#pragma omp parallel for
											for (int i = 0; i < DnaString::LITERAL.size() * 2; i++)												
											{
												char nextCh = DnaString::LITERAL[i / 2];
												DnaString posInEdge = posVertex;
												DnaString posOutEdge = posVertex;
												posInEdge.AppendFront(nextCh);
												posOutEdge.AppendBack(nextCh);
												DnaString negInEdge = negVertex;
												DnaString negOutEdge = negVertex;
												negInEdge.AppendBack(DnaString::Reverse(nextCh));
												negOutEdge.AppendFront(DnaString::Reverse(nextCh));
												assert(posInEdge.RevComp() == negInEdge);
												assert(posOutEdge.RevComp() == negOutEdge);
												if (i % 2 == 0 && (IsInBloomFilter(bitVector, seed, posInEdge) || IsInBloomFilter(bitVector, seed, negInEdge)))
												{
#pragma omp atomic
													inCount++;
												}

												if (i % 2 == 1 && (IsInBloomFilter(bitVector, seed, posOutEdge) || IsInBloomFilter(bitVector, seed, negOutEdge)))
												{
#pragma omp atomic
													outCount++;
												}
											}

											if (inCount > 1 || outCount > 1)
											{
												DnaString candidate(posVertex);
												candidate.AppendBack(posExtend);
												candidate.AppendBack(posPrev);
												candidateBifSet.insert(candidate.GetBody());
												if (posVertex == negVertex)
												{
													negFound = true;
												}
											}
										}

										if (posFound)
										{
											std::unordered_set<uint64_t, VertexHashFunction>::iterator it = candidateBifSet.find(posVertex.GetBody());
											DnaString candidate(vertexLength + 2, *it);
											char candExtend = candidate.GetChar(vertexLength);
											char candPrev = candidate.GetChar(vertexLength + 1);
											if ((candPrev != posPrev) || (candExtend != posExtend))
											{
												trueBifSet.insert(posVertex.GetBody());
												candidateBifSet.erase(posVertex.GetBody());
											}
										}

										if (negFound)
										{
											std::unordered_set<uint64_t, VertexHashFunction>::iterator it = candidateBifSet.find(negVertex.GetBody());
											if (it != candidateBifSet.end())
											{
												DnaString candidate(vertexLength + 2, *it);
												char candExtend = candidate.GetChar(vertexLength);
												char candPrev = candidate.GetChar(vertexLength + 1);
												if ((candPrev != DnaString::Reverse(posExtend)) || (candExtend != negExtend))
												{
													trueBifSet.insert(posVertex.GetBody());
													candidateBifSet.erase(posVertex.GetBody());
												}
											}
										}
									}
								}

							
								posVertex.AppendBack(posExtend);
								negVertex.AppendFront(DnaString::Reverse(posExtend));
								posPrev = posVertex.PopFront();
								negExtend = negVertex.PopBack();
							}
							else
							{
								if (hvalue >= low && hvalue <= high)
								{ 
									trueBifSet.insert(posVertex.GetBody());
								}

								break;
							}
						}
					}
				}
			}

			std::cout << "Round " << round << ", " << low << ":" << high << std::endl;
			std::cout << "Vertex count = " << trueBifSet.size() << std::endl;
			std::cout << "FP count = " << candidateBifSet.size() << std::endl;			
			for (uint64_t vertex : trueBifSet)
			{
				DnaString v(vertexLength, vertex);
				bifurcation_.push_back(v.GetBody());
			}

			low = high + 1;
		}
		
		std::sort(bifurcation_.begin(), bifurcation_.end());		*/
	}

	size_t VertexEnumerator::GetVerticesCount() const
	{
		return bifurcation_.size();
	}

	size_t VertexEnumerator::GetId(const DnaString & vertex) const
	{
		DnaString check[2] = { vertex, vertex.RevComp() };
		for (DnaString str : check)
		{
			std::vector<uint64_t>::const_iterator it = std::lower_bound(bifurcation_.begin(), bifurcation_.end(), str.GetBody());
			if (it != bifurcation_.end() && *it == str.GetBody())
			{
				return it - bifurcation_.begin();
			}
		}

		return INVALID_VERTEX;
	}
}