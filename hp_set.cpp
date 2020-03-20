#include <mutex>
#include <thread>
#include <iostream>
#include <chrono>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>

using namespace std;
using namespace chrono;

//static const int NUM_TEST = 4000000;
static const int NUM_TEST = 1000000;
static const int RANGE = 1000;

class LFNODE {
public:
	int key;
	unsigned next;

	LFNODE() {
		next = 0;
	}
	LFNODE(int x) {
		key = x;
		next = 0;
	}
	~LFNODE() {
	}
	LFNODE* GetNext() {
		return reinterpret_cast<LFNODE*>(next & 0xFFFFFFFE);
	}

	void SetNext(LFNODE* ptr) {
		next = reinterpret_cast<unsigned>(ptr);
	}

	LFNODE* GetNextWithMark(bool* mark) {
		int temp = next;
		*mark = (temp % 2) == 1;
		return reinterpret_cast<LFNODE*>(temp & 0xFFFFFFFE);
	}

	bool CAS(int old_value, int new_value)
	{
		return atomic_compare_exchange_strong(
			reinterpret_cast<atomic_int*>(&next),
			&old_value, new_value);
	}

	bool CAS(LFNODE* old_next, LFNODE* new_next, bool old_mark, bool new_mark) {
		unsigned old_value = reinterpret_cast<unsigned>(old_next);
		if (old_mark) old_value = old_value | 0x1;
		else old_value = old_value & 0xFFFFFFFE;
		unsigned new_value = reinterpret_cast<unsigned>(new_next);
		if (new_mark) new_value = new_value | 0x1;
		else new_value = new_value & 0xFFFFFFFE;
		return CAS(old_value, new_value);
	}

	bool TryMark(LFNODE* ptr)
	{
		unsigned old_value = reinterpret_cast<unsigned>(ptr) & 0xFFFFFFFE;
		unsigned new_value = old_value | 1;
		return CAS(old_value, new_value);
	}

	bool IsMarked() {
		return (0 != (next & 1));
	}
};

static const int max_thread = 16;
static const int max_hp = 3;
LFNODE* HP[max_thread][max_hp];
vector<LFNODE*> rlist[max_thread];
static int num_thread;
static int R = 3 * num_thread * 2;

void Scan(int tid) {
	auto i = rlist[tid].begin();
	while (i != rlist[tid].end()) {
		bool find = false;
		for (int t = 0; t < num_thread; ++t) {
			for (int h = 0; h < max_hp; ++h) {
				if (HP[t][h] == *i) {
					find = true;
					break;
				}
			}
			if (find) break;
		}
		if (find) {
			delete* i;
			if (&*i != &rlist[tid].back()) {
				*i = rlist[tid].back();
			}
			rlist[tid].pop_back();
		}
		else {
			++i;
		}
	}
}

class LFSET
{
	LFNODE head, tail;
public:
	LFSET()
	{
		head.key = 0x80000000;
		tail.key = 0x7FFFFFFF;
		head.SetNext(&tail);
	}

	void Retire(LFNODE *old, int tid)
	{
		rlist[tid].push_back(old);
		if (rlist[tid].size() >= R) {
			Scan(tid);
		}
	}

	void Init()
	{
		while (head.GetNext() != &tail) {
			LFNODE* temp = head.GetNext();
			head.next = temp->next;
			delete temp;
		}
	}

	void Dump()
	{
		LFNODE* ptr = head.GetNext();
		cout << "Result Contains : ";
		for (int i = 0; i < 20; ++i) {
			cout << ptr->key << ", ";
			if (&tail == ptr) break;
			ptr = ptr->GetNext();
		}
		cout << endl;
	}

	void Find(int x, LFNODE** pred, LFNODE** curr, int tid)
	{
	retry:
		HP[tid][0] = &head;
		do {
			HP[tid][1] = HP[tid][0]->GetNext();
			atomic_thread_fence(memory_order_seq_cst);
		} while (HP[tid][0]->GetNext() != HP[tid][1]);
		
		while (true) {
			bool removed;
			do {
				HP[tid][2] = HP[tid][1]->GetNextWithMark(&removed);
				atomic_thread_fence(memory_order_seq_cst);
			} while (HP[tid][1]->GetNext() != HP[tid][2]);

			while (true == removed) {
				if (false == HP[tid][0]->CAS(HP[tid][1], HP[tid][2], false, false))
					goto retry;
				Retire(HP[tid][1], tid);
				HP[tid][1] = HP[tid][2];
				do {
					HP[tid][2] = HP[tid][1]->GetNextWithMark(&removed);
					atomic_thread_fence(memory_order_seq_cst);
				} while (HP[tid][1]->GetNext() != HP[tid][2]);
			}
			if (HP[tid][1]->key >= x) {
				*pred = HP[tid][0]; *curr = HP[tid][1];
				HP[tid][2] = nullptr;
				return;
			}
			HP[tid][0] = HP[tid][1];
			HP[tid][1] = HP[tid][2];
		}
	}
	bool Add(int x, int tid)
	{
		LFNODE* pred, * curr;
		LFNODE* e = new LFNODE(x);

		while (true) {
			HP[tid][0] = nullptr;
			HP[tid][1] = nullptr;
			HP[tid][2] = nullptr;
			Find(x, &pred, &curr, tid);
			if (curr->key == x) {
				delete e;
				HP[tid][0] = nullptr;
				HP[tid][1] = nullptr;
				return false;
			}
			else {
				e->SetNext(curr);
				if (false == pred->CAS(curr, e, false, false))
					continue;
				HP[tid][0] = nullptr;
				HP[tid][1] = nullptr;
				return true;
			}
		}
	}
	bool Remove(int x, int tid)
	{
		LFNODE* pred, * curr;
		while (true) {
			HP[tid][0] = nullptr;
			HP[tid][1] = nullptr;
			HP[tid][2] = nullptr;
			Find(x, &pred, &curr, tid);

			if (curr->key != x) {
				HP[tid][0] = nullptr;
				HP[tid][1] = nullptr;
				return false;
			}
			else {
				// Try Mark 에서 succ remove 되지 않았다는 것 보장
				LFNODE* succ = curr->GetNext();
				if (false == curr->TryMark(succ)) continue;
				if (pred->CAS(curr, succ, false, false)) {
					Retire(curr, tid);
				}
				HP[tid][0] = nullptr;
				HP[tid][1] = nullptr;
				//delete curr;
				return true;
			}
		}
	}
	bool Contains(int x, int tid)
	{
		HP[tid][0] = &head;
		HP[tid][1] = &head;
		while (HP[tid][1]->key < x) {
			do {
				HP[tid][1] = HP[tid][0]->GetNext();
				atomic_thread_fence(memory_order_seq_cst);
			} while (HP[tid][0]->GetNext() != HP[tid][1]);
			HP[tid][0] = HP[tid][1];
		}

		bool ret = ((false == HP[tid][1]->IsMarked()) && (x == HP[tid][1]->key));
		HP[tid][0] = nullptr;
		HP[tid][1] = nullptr;
		return ret;
	}
};

LFSET my_set;

void benchmark(int num_thread, int tid)
{
	for (int i = 0; i < NUM_TEST / num_thread; ++i) {
		//	if (0 == i % 100000) cout << ".";
		switch (rand() % 3) {
		case 0: my_set.Add(rand() % RANGE, tid); break;
		case 1: my_set.Remove(rand() % RANGE, tid); break;
		case 2: my_set.Contains(rand() % RANGE, tid); break;
		default: cout << "ERROR!!!\n"; exit(-1);
		}
	}

	//rlist 비우기
}

int main()
{
	vector <thread> worker;
	for (int num_threads = 1; num_threads <= 16; num_threads *= 2) {
		num_thread = num_threads;
		my_set.Init();
		worker.clear();
		for (int rt = 0; rt < max_thread; ++rt) {
			for (auto nd : rlist[rt]) {
				delete nd;
			}
			rlist[rt].clear();
		}

		auto start_t = high_resolution_clock::now();
		for (int i = 0; i < num_threads; ++i)
			worker.push_back(thread{ benchmark, num_threads, i });
		for (auto& th : worker) th.join();
		auto du = high_resolution_clock::now() - start_t;
		my_set.Dump();

		cout << num_threads << " Threads,  Time = ";
		cout << duration_cast<milliseconds>(du).count() << " ms\n";
	}
	system("pause");
}
