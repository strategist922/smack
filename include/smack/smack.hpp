#ifndef __SMACK_SMACK_HPP
#define __SMACK_SMACK_HPP

#include <algorithm>
#include <deque>

#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/thread/condition.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filter/bzip2.hpp>
#include <boost/thread.hpp>

#include <smack/base.hpp>
#include <smack/blob.hpp>
#include <smack/snappy.hpp>
#include <smack/lz4.hpp>

namespace ioremap { namespace smack {

namespace fs = boost::filesystem;

template <class fout_t, class fin_t>
class cache_processor {
	public:
		cache_processor(int thread_num) : need_exit_(0), processed_(0) {
			for (int i = 0; i < thread_num; ++i)
				group_.create_thread(boost::bind(&cache_processor::process, this));
		}

		~cache_processor() {
			need_exit_ = 1;
			cond_.notify_all();
			group_.join_all();

			log(SMACK_LOG_INFO, "cache processor completed\n");
		}


		void notify(boost::shared_ptr<blob<fout_t, fin_t> > b) {
			boost::mutex::scoped_lock guard(lock_);
			typename std::deque<boost::shared_ptr<blob<fout_t, fin_t> > >::iterator it = std::find(blobs_.begin(), blobs_.end(), b);
			if (it == blobs_.end())
				blobs_.push_back(b);

			cond_.notify_all();
		}

		void wait_for_all() {
			boost::mutex::scoped_lock guard(lock_);

			while ((processed_ || !blobs_.empty()) && !need_exit_) {
				cond_.wait(guard);
			}
		}

	private:
		boost::mutex lock_;
		boost::condition cond_;
		std::deque<boost::shared_ptr<blob<fout_t, fin_t> > > blobs_;
		boost::thread_group group_;
		int need_exit_;
		int processed_;

		void process(void) {
			while (!need_exit_) {
				boost::shared_ptr<blob<fout_t, fin_t> > b;

				{
					boost::mutex::scoped_lock guard(lock_);

					while (blobs_.empty() && !need_exit_) {
						cond_.wait(guard);
					}

					if (need_exit_)
						break;

					if (blobs_.empty())
						continue;

					b = blobs_.front();
					blobs_.pop_front();

					processed_++;
				}

				while (b->write_cache()) ;
				boost::mutex::scoped_lock guard(lock_);
				processed_--;
				cond_.notify_all();
			}
		}

};


template <class fout_t, class fin_t>
class smack {
	public:
		smack(		const std::string &path,
				int bloom_size = 1024,
				size_t max_cache_size = 10000,
				int max_blob_num = 100,
				int cache_thread_num = 10) :
			m_need_exit(false),
			path_base_(path), bloom_size_(bloom_size), blob_num_(0),
			max_cache_size_(max_cache_size), max_blob_num_(max_blob_num), proc_(cache_thread_num) {
			if (!fs::exists(path))
				throw std::runtime_error("Directory " + path + " does not exist");

			std::vector<std::string> blobs;

			fs::directory_iterator end_itr;
			for (fs::directory_iterator itr(path); itr != end_itr; ++itr) {
				fs::path p(*itr);

				if (!fs::is_regular_file(p))
					continue;

				int num;
				if (sscanf(p.filename().c_str(), "smack.%d.", &num) == 1) {
					std::vector<std::string>::iterator sit;
					std::string tmp = "smack." + boost::lexical_cast<std::string>(num);

					for (sit = blobs.begin(); sit != blobs.end(); ++sit) {
						if (*sit == tmp)
							break;
					}
					if (sit != blobs.end())
						continue;

					blobs.push_back(tmp);

					std::string file = path + "/" + tmp;
					log(SMACK_LOG_NOTICE, "open: %s\n", file.c_str());

					boost::shared_ptr<blob<fout_t, fin_t> > b(new blob<fout_t, fin_t>(file, bloom_size, max_cache_size));
					blobs_.insert(std::make_pair(b->start(), b));

					if (num > blob_num_)
						blob_num_ = num;

					b->set_want_rcache(true);
					b->set_want_resort(b->have_unsorted_chunks() > 0);
					proc_.notify(b);
				}
			}

			if (blobs_.size() == 0)
				blobs_.insert(std::make_pair(key(),
						boost::shared_ptr<blob<fout_t, fin_t> >(
							new blob<fout_t, fin_t>(path + "/smack.0", bloom_size, max_cache_size))));

			m_sync_thread = boost::thread(boost::bind(&smack::run_sync, this));
		}

		virtual ~smack() {
			m_need_exit = true;
			sync();
		}

		void write(const key &key, const char *data, size_t size) {
			boost::shared_ptr<blob<fout_t, fin_t> > curb = blob_lookup(key, false);

			if (curb->write(key, data, size)) {
				boost::mutex::scoped_lock guard(m_blobs_lock);

				size_t data_size, num;
				bool have_split;

				curb->disk_stat(num, data_size, have_split);

				if ((blobs_.size() < max_blob_num_) &&
						(data_size > 10 * 1024 * 1024) &&
						!have_split) {
					blob_num_++;
					boost::shared_ptr<blob<fout_t, fin_t> >	b(new blob<fout_t, fin_t>(
								path_base_ + "/smack." + boost::lexical_cast<std::string>(blob_num_),
								bloom_size_, max_cache_size_));

					curb->set_split_dst(b);

					blobs_.insert(std::make_pair(b->start(), b));
				}

				proc_.notify(curb);
			}
		}

		std::string read(key &key) {
			return blob_lookup(key, true)->read(key);
		}

		void remove(const key &key) {
			boost::shared_ptr<blob<fout_t, fin_t> > curb = blob_lookup(key, true);
			if (curb->remove(key))
				proc_.notify(curb);
		}

		void sync(void) {
			for (typename std::map<key, boost::shared_ptr<blob<fout_t, fin_t> >, keycomp>::iterator it = blobs_.begin();
					it != blobs_.end(); ++it) {
				proc_.notify(it->second);
			}

			proc_.wait_for_all();
		}

		std::string lookup(key &k) {
			boost::shared_ptr<blob<fout_t, fin_t> > curb = blob_lookup(k, true);
			return curb->lookup(k);
		}

		long long total_num() {
			boost::mutex::scoped_lock guard(m_blobs_lock);

			long long total_num = 0;
			for (typename std::map<key, boost::shared_ptr<blob<fout_t, fin_t> >, keycomp>::iterator it = blobs_.begin();
					it != blobs_.end(); ++it) {
				size_t num, size;
				bool have_split;

				it->second->disk_stat(num, size, have_split);
				total_num += num;
			}

			return total_num;
		}

	private:
		std::map<key, boost::shared_ptr<blob<fout_t, fin_t> >, keycomp> blobs_;
		bool m_need_exit;
		boost::mutex m_blobs_lock;
		std::string path_base_;
		int bloom_size_;
		int blob_num_;
		size_t max_cache_size_;
		size_t max_blob_num_;
		cache_processor<fout_t, fin_t> proc_;
		boost::thread m_sync_thread;

		boost::shared_ptr<blob<fout_t, fin_t> > blob_lookup(const key &k, bool check_start_key = false) {
			boost::mutex::scoped_lock guard(m_blobs_lock);

			if (blobs_.size() == 0)
				throw std::out_of_range("smack::blob-lookup::no-blobs");
			
			boost::shared_ptr<blob<fout_t, fin_t> > b;

			typename std::map<key, boost::shared_ptr<blob<fout_t, fin_t> >, keycomp>::iterator it = blobs_.upper_bound(k);
			if (it == blobs_.end()) {
				b = blobs_.rbegin()->second;
			} else if (it == blobs_.begin()) {
				b = it->second;
			} else {
				b = (--it)->second;
			}

			if (check_start_key && (b->start() > k))
				throw std::out_of_range("smack::blob-lookup::start-key");

			return b;
		}

		void run_sync() {
			int m_sync_timeout = 60;

			while (!m_need_exit) {
				for (int i = 0; i < m_sync_timeout; ++i) {
					if (m_need_exit)
						break;

					sleep(1);
				}

				if (!m_need_exit)
					sync();
			}
		}
};

class zlib_max_compression_compressor : public boost::iostreams::zlib_compressor {
	public:
		zlib_max_compression_compressor() : boost::iostreams::zlib_compressor(
				boost::iostreams::zlib_params(boost::iostreams::zlib::best_compression)) {}
};

class zlib_max_compression_decompressor : public boost::iostreams::zlib_decompressor {
	public:
		zlib_max_compression_decompressor() : boost::iostreams::zlib_decompressor(
				boost::iostreams::zlib_params(boost::iostreams::zlib::best_compression)) {}
};

typedef smack<zlib_max_compression_compressor, zlib_max_compression_decompressor> smack_zlib_best;
typedef smack<boost::iostreams::zlib_compressor, boost::iostreams::zlib_decompressor> smack_zlib_default;
typedef smack<boost::iostreams::bzip2_compressor, boost::iostreams::bzip2_decompressor> smack_bzip2;
typedef smack<snappy::snappy_compressor, snappy::snappy_decompressor> smack_snappy;
typedef smack<lz4::fast_compressor, lz4::decompressor> smack_lz4_fast;
typedef smack<lz4::high_compressor, lz4::decompressor> smack_lz4_high;

}}

#endif /* __SMACK_SMACK_HPP */
