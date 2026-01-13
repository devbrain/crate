#include <crate/compression/rar_ppm.hh>

namespace crate::rar::ppm {
    sub_allocator::~sub_allocator() { stop(); }
    void sub_allocator::start(int size_mb) {
        stop();
        size_t size = static_cast<size_t>(size_mb) << 20;
        heap_.resize(size);
        heap_size_ = size;
        init();
    }
    void sub_allocator::stop() {
        // Free all allocated units
        for (void* p : allocated_) {
            ::operator delete(p);
        }
        allocated_.clear();
        heap_.clear();
        heap_size_ = 0;
        text_pos_ = 0;
    }
    void sub_allocator::init() {
        text_pos_ = 0;
        // Free previously allocated units for fresh start
        for (void* p : allocated_) {
            ::operator delete(p);
        }
        allocated_.clear();
    }
    u8* sub_allocator::text() {
        return heap_.data() + text_pos_;
    }
    u8* sub_allocator::text_start() {
        return heap_.data();
    }
    u8* sub_allocator::fake_units_start() {
        return heap_.data() + (heap_size_ * 7 / 8);
    }
    u8* sub_allocator::heap_end() {
        return heap_.data() + heap_size_;
    }
    void sub_allocator::advance_text(size_t n) {
        text_pos_ += n;
    }
    void sub_allocator::retreat_text(size_t n) {
        text_pos_ -= n;
    }
    void* sub_allocator::alloc_units(int count) {
        size_t bytes = static_cast<size_t>(count) * sizeof(state);
        void* p = ::operator new(bytes, std::nothrow);
        if (p) {
            allocated_.push_back(p);
            std::memset(p, 0, bytes);
        }
        return p;
    }
    void sub_allocator::free_units(void* ptr, int) {
        // In a more optimized version, we'd add to a free list
        // For now, memory is freed when SubAllocator is reset
        (void)ptr;
    }
    void* sub_allocator::expand_units(void* old_ptr, int old_count) {
        void* new_ptr = alloc_units(old_count + 1);
        if (new_ptr && old_ptr) {
            std::memcpy(new_ptr, old_ptr, static_cast<size_t>(old_count) * sizeof(state));
        }
        return new_ptr;
    }
    void* sub_allocator::shrink_units(void* ptr, int, int) {
        return ptr;
    }
    long sub_allocator::allocated_memory() const {
        return static_cast<long>(heap_size_);
    }
    void see2_context::init(int init_val) {
        summ = static_cast<u16>(init_val << (shift = PERIOD_BITS - 4));
        count = 4;
    }
    unsigned see2_context::get_mean() {
        int ret_val = static_cast<i16>(summ) >> shift;
        summ -= static_cast<u16>(ret_val);
        unsigned result = static_cast<unsigned>(ret_val);
        return result + static_cast<unsigned>(result == 0);
    }
    void see2_context::update() {
        if (shift < PERIOD_BITS && --count == 0) {
            summ += summ;
            count = 3 << shift++;
        }
    }
    } // namespace crate::rar::ppm
