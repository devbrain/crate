#include <doctest/doctest.h>
#include <crate/test_config.hh>
#include <crate/core/system.hh>
#include <crate/compression/streams.hh>
#include <crate/compression/inflate.hh>
#include <crate/formats/zip.hh>
#include <crate/formats/arj.hh>
#include <crate/formats/arc.hh>
#include <crate/formats/cab.hh>
#include <crate/formats/lha.hh>
#include <crate/formats/ha.hh>
#include <crate/formats/hyp.hh>
#include <crate/formats/zoo.hh>
#include <crate/formats/rar.hh>
#include <crate/formats/ace.hh>
#include <crate/formats/chm.hh>
#include <crate/formats/floppy.hh>
#include <crate/formats/stuffit.hh>
#ifdef CRATE_WITH_LIBARCHIVE
#include <crate/formats/libarchive_archive.hh>
#endif
#include <sstream>
#include <fstream>

using namespace crate;

namespace {
    const auto TESTDATA_DIR = test::testdata_dir();
    const auto ZIP_DIR = test::zip_dir();
    const auto ARJ_DIR = test::arj_dir();
    const auto ARC_DIR = test::arc_dir();
    const auto CAB_DIR = test::cab_dir();
    const auto LHA_DIR = test::lha_dir();
    const auto HA_DIR = test::ha_dir();
    const auto HYP_DIR = test::hyp_dir();
    const auto ZOO_DIR = test::zoo_dir();
    const auto RAR_DIR = test::rar_dir();
    const auto ACE_DIR = test::ace_dir();
    const auto CHM_DIR = test::chm_dir();
    const auto FLOPPY_DIR = test::floppy_dir();
    const auto STUFFIT_DIR = test::stuffit_dir();
    const auto GZ_DIR = test::testdata_dir() / "gz";
    const auto BZ2_DIR = test::bzip2_dir();
    const auto XZ_DIR = test::xz_dir();
    const auto ZSTD_DIR = test::zstd_dir();
    const auto BROTLI_DIR = test::brotli_dir();
    const auto LICENSE_FILE = test::testdata_dir() / "LICENSE";

    // Helper: read all bytes from an istream into a string
    std::string read_all(std::istream& is) {
        return std::string((std::istreambuf_iterator<char>(is)),
                          std::istreambuf_iterator<char>());
    }

    // Helper: read a file into a byte_vector
    byte_vector read_file(const std::filesystem::path& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};
        return byte_vector((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    }

    // Helper: verify extract_stream matches extract for every file in an archive.
    // Requires at least one file to actually be verified (no vacuous passes).
    void verify_extract_stream(crate::archive& ar, const char* label) {
        size_t checked = 0;
        for (const auto& entry : ar.files()) {
            if (entry.is_directory) continue;

            auto content = ar.extract(entry);
            if (!content.has_value()) continue; // skip encrypted / unsupported entries

            auto stream = ar.extract_stream(entry);
            REQUIRE_MESSAGE(stream.has_value(),
                label << ": extract_stream failed for " << entry.name);

            std::string from_extract(content->begin(), content->end());
            std::string from_stream = read_all(**stream);
            CHECK_MESSAGE(from_extract == from_stream,
                label << ": mismatch for " << entry.name
                      << " (extract=" << from_extract.size()
                      << " stream=" << from_stream.size() << ")");
            ++checked;
        }
        REQUIRE_MESSAGE(checked > 0, label << ": no files were verified — test is vacuous");
    }

    // Helper: verify open(path) and open(istream) produce identical results.
    // ArchiveT must have open(path) and open(istream&).
    template<typename ArchiveT>
    void verify_path_vs_istream(const std::filesystem::path& path, const char* label) {
        auto archive_path = ArchiveT::open(path);
        REQUIRE_MESSAGE(archive_path.has_value(), label << ": open(path) failed");

        std::ifstream file(path, std::ios::binary);
        REQUIRE(file.good());
        auto archive_stream = ArchiveT::open(file);
        REQUIRE_MESSAGE(archive_stream.has_value(), label << ": open(istream) failed");

        auto& files_path = (*archive_path)->files();
        auto& files_stream = (*archive_stream)->files();
        REQUIRE_MESSAGE(files_path.size() == files_stream.size(),
            label << ": file count differs (path=" << files_path.size()
                  << " stream=" << files_stream.size() << ")");

        size_t compared = 0;
        for (size_t i = 0; i < files_path.size(); ++i) {
            CHECK(files_path[i].name == files_stream[i].name);
            CHECK(files_path[i].uncompressed_size == files_stream[i].uncompressed_size);

            if (files_path[i].is_directory) continue;

            auto content_path = (*archive_path)->extract(files_path[i]);
            auto content_stream = (*archive_stream)->extract(files_stream[i]);
            if (!content_path.has_value()) continue; // skip unextractable
            REQUIRE_MESSAGE(content_stream.has_value(),
                label << ": stream extract failed for " << files_stream[i].name);
            CHECK_MESSAGE(*content_path == *content_stream,
                label << ": content differs for " << files_path[i].name);
            ++compared;
        }
        REQUIRE_MESSAGE(compared > 0, label << ": no files compared — test is vacuous");
    }

    // A streambuf that is non-seekable (returns -1 for tellg).
    // Wraps a string and only supports sequential reading.
    class non_seekable_streambuf : public std::streambuf {
    public:
        explicit non_seekable_streambuf(const std::string& data)
            : data_(data), pos_(0) {}

    protected:
        int_type underflow() override {
            if (pos_ >= data_.size()) return traits_type::eof();
            return traits_type::to_int_type(data_[pos_]);
        }

        int_type uflow() override {
            if (pos_ >= data_.size()) return traits_type::eof();
            return traits_type::to_int_type(data_[pos_++]);
        }

        std::streamsize showmanyc() override {
            return static_cast<std::streamsize>(data_.size() - pos_);
        }

        // Explicitly disable seeking
        pos_type seekoff(off_type, std::ios_base::seekdir,
                         std::ios_base::openmode) override {
            return pos_type(off_type(-1));
        }
        pos_type seekpos(pos_type, std::ios_base::openmode) override {
            return pos_type(off_type(-1));
        }

    private:
        std::string data_;
        size_t pos_;
    };

    // A streambuf that simulates a read error after N bytes.
    // After producing N bytes, throws std::ios_base::failure which causes
    // the owning istream to set badbit.
    class error_after_n_streambuf : public std::streambuf {
    public:
        explicit error_after_n_streambuf(size_t n)
            : limit_(n), produced_(0) {}

    protected:
        int_type underflow() override {
            if (produced_ >= limit_) {
                throw std::ios_base::failure("simulated read error");
            }
            ch_ = 'X';
            setg(&ch_, &ch_, &ch_ + 1);
            return traits_type::to_int_type(ch_);
        }

        int_type uflow() override {
            if (produced_ >= limit_) {
                throw std::ios_base::failure("simulated read error");
            }
            ch_ = 'X';
            ++produced_;
            setg(&ch_, &ch_ + 1, &ch_ + 1);
            return traits_type::to_int_type(ch_);
        }

    private:
        size_t limit_;
        size_t produced_;
        char ch_;
    };
}

// ============================================================================
// read_stream
// ============================================================================

TEST_SUITE("read_stream") {
    TEST_CASE("Read from string stream") {
        std::istringstream ss("Hello, World!");
        auto result = read_stream(ss);
        REQUIRE(result.has_value());
        std::string text(result->begin(), result->end());
        CHECK(text == "Hello, World!");
    }

    TEST_CASE("Read empty stream") {
        std::istringstream ss("");
        auto result = read_stream(ss);
        REQUIRE(result.has_value());
        CHECK(result->empty());
    }

    TEST_CASE("Read from file stream") {
        auto path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        REQUIRE(file.good());

        auto result = read_stream(file);
        REQUIRE(result.has_value());
        CHECK(result->size() == std::filesystem::file_size(path));
    }

    TEST_CASE("Read from error stream returns ReadError") {
        // Produce 100 bytes then simulate I/O error via exception
        error_after_n_streambuf buf(100);
        std::istream is(&buf);

        auto result = read_stream(is);
        // The stream should have set badbit when the streambuf threw
        CHECK(is.bad());
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code() == error_code::ReadError);
    }

    TEST_CASE("Read from non-seekable stream") {
        std::string data = "The quick brown fox jumps over the lazy dog";
        non_seekable_streambuf buf(data);
        std::istream is(&buf);

        // tellg should return -1 for non-seekable streams
        CHECK(is.tellg() == std::istream::pos_type(-1));

        auto result = read_stream(is);
        REQUIRE(result.has_value());
        std::string text(result->begin(), result->end());
        CHECK(text == data);
    }

    TEST_CASE("Read from non-seekable stream with large data") {
        // Create data larger than the 64KB internal buffer to exercise
        // multiple read iterations on the non-seekable path
        std::string data(100'000, 'A');
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<char>('A' + (i % 26));

        non_seekable_streambuf buf(data);
        std::istream is(&buf);

        auto result = read_stream(is);
        REQUIRE(result.has_value());
        CHECK(result->size() == data.size());
        std::string text(result->begin(), result->end());
        CHECK(text == data);
    }

    TEST_CASE("Read from stream at non-zero position") {
        auto path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        REQUIRE(file.good());

        auto full_size = std::filesystem::file_size(path);
        REQUIRE(full_size > 10);

        // Seek 10 bytes in, then read_stream should read only the remainder
        file.seekg(10);
        auto result = read_stream(file);
        REQUIRE(result.has_value());
        CHECK(result->size() == full_size - 10);
    }
}

// ============================================================================
// byte_vector_istream
// ============================================================================

TEST_SUITE("byte_vector_istream") {
    TEST_CASE("Read from byte_vector_istream") {
        byte_vector data = {'H', 'e', 'l', 'l', 'o'};
        byte_vector_istream stream(std::move(data));

        std::string text = read_all(stream);
        CHECK(text == "Hello");
    }

    TEST_CASE("Empty byte_vector_istream") {
        byte_vector_istream stream(byte_vector{});
        CHECK(stream.peek() == std::char_traits<char>::eof());
    }

    TEST_CASE("Seek in byte_vector_istream") {
        byte_vector data = {'A', 'B', 'C', 'D', 'E'};
        byte_vector_istream stream(std::move(data));

        char c;

        // Seek absolute
        stream.seekg(3);
        stream.get(c);
        CHECK(c == 'D');

        // Seek from current (+1 skips 'E', so from pos 4 + (-3) = 1)
        stream.seekg(-3, std::ios::cur);
        stream.get(c);
        CHECK(c == 'B');

        // Seek from end
        stream.seekg(-2, std::ios::end);
        stream.get(c);
        CHECK(c == 'D');

        // Seek to beginning
        stream.seekg(0);
        stream.get(c);
        CHECK(c == 'A');
    }
}

// ============================================================================
// open(std::istream&) on archive classes — using specific known-good files
// ============================================================================

TEST_SUITE("Archive open(istream)") {
    TEST_CASE("ZIP: open from istream") {
        auto path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        REQUIRE(file.good());

        auto archive = zip_archive::open(file);
        REQUIRE(archive.has_value());

        auto& files = (*archive)->files();
        REQUIRE(files.size() == 1);
        CHECK(files[0].name == "hello.txt");

        auto content = (*archive)->extract(files[0]);
        REQUIRE(content.has_value());
        std::string text(content->begin(), content->end());
        CHECK(text == "Hello, World!\n");
    }

    TEST_CASE("ZIP: open deflated from istream") {
        auto path = ZIP_DIR / "deflated.zip";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto archive = zip_archive::open(file);
        REQUIRE(archive.has_value());

        auto& files = (*archive)->files();
        REQUIRE(files.size() >= 1);

        auto content = (*archive)->extract(files[0]);
        REQUIRE(content.has_value());
        std::string text(content->begin(), content->end());
        CHECK(text == "This is a test file for ZIP archive testing.\n");
    }

    TEST_CASE("ARJ: open from istream") {
        auto path = ARJ_DIR / "stored.arj";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto archive = arj_archive::open(file);
        REQUIRE(archive.has_value());
        CHECK(!(*archive)->files().empty());

        for (const auto& entry : (*archive)->files()) {
            if (entry.is_directory) continue;
            auto content = (*archive)->extract(entry);
            CHECK(content.has_value());
        }
    }

    TEST_CASE("CAB: open from istream") {
        auto path = CAB_DIR / "simple.cab";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto archive = cab_archive::open(file);
        REQUIRE(archive.has_value());
        CHECK(!(*archive)->files().empty());
    }

    TEST_CASE("ARC: open from istream") {
        auto path = ARC_DIR / "store.arc";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto archive = arc_archive::open(file);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());
        auto content = (*archive)->extract((*archive)->files()[0]);
        CHECK(content.has_value());
    }

    TEST_CASE("LHA: open from istream") {
        auto path = LHA_DIR / "lha255e" / "lh5.lzh";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto archive = lha_archive::open(file);
        REQUIRE(archive.has_value());
        CHECK(!(*archive)->files().empty());
    }

    TEST_CASE("ZOO: open from istream") {
        auto path = ZOO_DIR / "store.zoo";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto archive = zoo_archive::open(file);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());
        auto content = (*archive)->extract((*archive)->files()[0]);
        CHECK(content.has_value());
    }

    TEST_CASE("RAR: open from istream") {
        auto path = RAR_DIR / "unrar_test_01.rar";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto archive = rar_archive::open(file);
        REQUIRE(archive.has_value());
        CHECK(!(*archive)->files().empty());
    }

    TEST_CASE("ACE: open from istream") {
        auto path = ACE_DIR / "license1.ace";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto archive = ace_archive::open(file);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());
        auto content = (*archive)->extract((*archive)->files()[0]);
        CHECK(content.has_value());
    }

    TEST_CASE("HA: open from istream") {
        auto path = HA_DIR / "copy.ha";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto archive = ha_archive::open(file);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());
        auto content = (*archive)->extract((*archive)->files()[0]);
        CHECK(content.has_value());
    }

    TEST_CASE("HYP: open from istream") {
        auto path = HYP_DIR / "stored.hyp";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto archive = hyp_archive::open(file);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());
        auto content = (*archive)->extract((*archive)->files()[0]);
        CHECK(content.has_value());
    }

    TEST_CASE("CHM: open from istream") {
        auto path = CHM_DIR / "main.chm";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto archive = chm_archive::open(file);
        REQUIRE(archive.has_value());
        CHECK(!(*archive)->files().empty());
    }

    TEST_CASE("Floppy: open from istream") {
        auto path = FLOPPY_DIR / "Borland - Turbo Pascal v5.0 - Disk 1 of 3 - Install & Compiler.img";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto archive = floppy_image::open(file);
        REQUIRE(archive.has_value());
        CHECK(!(*archive)->files().empty());
    }

    TEST_CASE("StuffIt: open from istream") {
        auto path = STUFFIT_DIR / "test_m0.sit";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto archive = stuffit_archive::open(file);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());
        auto content = (*archive)->extract((*archive)->files()[0]);
        CHECK(content.has_value());
    }

#ifdef CRATE_WITH_LIBARCHIVE
    TEST_CASE("libarchive: open from istream") {
        auto path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto archive = libarchive_archive::open(file);
        REQUIRE(archive.has_value());
        auto& files = (*archive)->files();
        REQUIRE(!files.empty());
        auto content = (*archive)->extract(files[0]);
        CHECK(content.has_value());
    }
#endif
}

// ============================================================================
// open(istream) vs open(path) equivalence
// ============================================================================

TEST_SUITE("Archive open(istream) equivalence") {
    TEST_CASE("ZIP: path vs istream equivalence") {
        auto path = ZIP_DIR / "multiple.zip";
        REQUIRE(std::filesystem::exists(path));
        verify_path_vs_istream<zip_archive>(path, "ZIP-multiple");
    }

    TEST_CASE("ARJ: path vs istream equivalence") {
        auto path = ARJ_DIR / "stored.arj";
        REQUIRE(std::filesystem::exists(path));
        verify_path_vs_istream<arj_archive>(path, "ARJ-stored");
    }

    TEST_CASE("ARC: path vs istream equivalence") {
        auto path = ARC_DIR / "store.arc";
        REQUIRE(std::filesystem::exists(path));
        verify_path_vs_istream<arc_archive>(path, "ARC-store");
    }

    TEST_CASE("CAB: path vs istream equivalence") {
        auto path = CAB_DIR / "simple.cab";
        REQUIRE(std::filesystem::exists(path));
        verify_path_vs_istream<cab_archive>(path, "CAB-simple");
    }

    TEST_CASE("LHA: path vs istream equivalence") {
        auto path = LHA_DIR / "lha255e" / "lh5.lzh";
        REQUIRE(std::filesystem::exists(path));
        verify_path_vs_istream<lha_archive>(path, "LHA-lh5");
    }

    TEST_CASE("ZOO: path vs istream equivalence") {
        auto path = ZOO_DIR / "store.zoo";
        REQUIRE(std::filesystem::exists(path));
        verify_path_vs_istream<zoo_archive>(path, "ZOO-store");
    }

    TEST_CASE("RAR: path vs istream equivalence") {
        auto path = RAR_DIR / "unrar_test_01.rar";
        REQUIRE(std::filesystem::exists(path));
        verify_path_vs_istream<rar_archive>(path, "RAR-test01");
    }

    TEST_CASE("HA: path vs istream equivalence") {
        auto path = HA_DIR / "copy.ha";
        REQUIRE(std::filesystem::exists(path));
        verify_path_vs_istream<ha_archive>(path, "HA-copy");
    }

    TEST_CASE("HYP: path vs istream equivalence") {
        auto path = HYP_DIR / "stored.hyp";
        REQUIRE(std::filesystem::exists(path));
        verify_path_vs_istream<hyp_archive>(path, "HYP-stored");
    }

    TEST_CASE("CHM: path vs istream equivalence") {
        auto path = CHM_DIR / "main.chm";
        REQUIRE(std::filesystem::exists(path));
        verify_path_vs_istream<chm_archive>(path, "CHM-main");
    }

    TEST_CASE("Floppy: path vs istream equivalence") {
        auto path = FLOPPY_DIR / "Borland - Turbo Pascal v5.0 - Disk 1 of 3 - Install & Compiler.img";
        REQUIRE(std::filesystem::exists(path));
        verify_path_vs_istream<floppy_image>(path, "Floppy-disk1");
    }

#ifdef CRATE_WITH_LIBARCHIVE
    TEST_CASE("libarchive: path vs istream equivalence") {
        auto path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(path));
        verify_path_vs_istream<libarchive_archive>(path, "libarchive-zip");
    }
#endif
}

// ============================================================================
// open(istream) with stream at non-zero position
// ============================================================================

TEST_SUITE("Archive open(istream) non-zero offset") {
    TEST_CASE("open(istream) at non-zero position reads from current pos") {
        // Create a file that has a 16-byte garbage prefix before a valid zip
        auto zip_path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(zip_path));

        auto zip_data = read_file(zip_path);
        REQUIRE(!zip_data.empty());

        // Prepend 16 garbage bytes
        byte_vector combined(16, byte{0xAB});
        combined.insert(combined.end(), zip_data.begin(), zip_data.end());

        // Write to a temp file
        auto tmp_path = std::filesystem::temp_directory_path() / "test_offset.bin";
        {
            std::ofstream out(tmp_path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(combined.data()),
                      static_cast<std::streamsize>(combined.size()));
        }

        // Open and seek past the prefix
        std::ifstream file(tmp_path, std::ios::binary);
        file.seekg(16);

        auto archive = zip_archive::open(file);
        REQUIRE(archive.has_value());
        auto& files = (*archive)->files();
        REQUIRE(files.size() == 1);
        CHECK(files[0].name == "hello.txt");

        auto content = (*archive)->extract(files[0]);
        REQUIRE(content.has_value());
        std::string text(content->begin(), content->end());
        CHECK(text == "Hello, World!\n");

        std::filesystem::remove(tmp_path);
    }
}

// ============================================================================
// extract_stream — verify across all archive formats
// ============================================================================

TEST_SUITE("extract_stream") {
    TEST_CASE("ZIP stored: extract_stream content") {
        auto path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(path));

        auto archive = zip_archive::open(path);
        REQUIRE(archive.has_value());

        auto& files = (*archive)->files();
        REQUIRE(!files.empty());

        auto stream = (*archive)->extract_stream(files[0]);
        REQUIRE(stream.has_value());
        CHECK(read_all(**stream) == "Hello, World!\n");
    }

    TEST_CASE("ZIP deflated: extract_stream matches extract") {
        auto path = ZIP_DIR / "deflated.zip";
        REQUIRE(std::filesystem::exists(path));

        auto ar = zip_archive::open(path);
        REQUIRE(ar.has_value());
        verify_extract_stream(**ar, "ZIP-deflated");
    }

    TEST_CASE("ZIP multiple files: extract_stream matches extract") {
        auto path = ZIP_DIR / "multiple.zip";
        REQUIRE(std::filesystem::exists(path));

        auto ar = zip_archive::open(path);
        REQUIRE(ar.has_value());
        verify_extract_stream(**ar, "ZIP-multiple");
    }

    TEST_CASE("ARJ: extract_stream matches extract") {
        auto path = ARJ_DIR / "stored.arj";
        REQUIRE(std::filesystem::exists(path));

        auto ar = arj_archive::open(path);
        REQUIRE(ar.has_value());
        verify_extract_stream(**ar, "ARJ-stored");
    }

    TEST_CASE("ARC: extract_stream matches extract") {
        auto path = ARC_DIR / "store.arc";
        REQUIRE(std::filesystem::exists(path));

        auto ar = arc_archive::open(path);
        REQUIRE(ar.has_value());
        verify_extract_stream(**ar, "ARC-store");
    }

    TEST_CASE("CAB: extract_stream matches extract") {
        auto path = CAB_DIR / "simple.cab";
        REQUIRE(std::filesystem::exists(path));

        auto ar = cab_archive::open(path);
        REQUIRE(ar.has_value());
        verify_extract_stream(**ar, "CAB-simple");
    }

    TEST_CASE("LHA: extract_stream matches extract") {
        auto path = LHA_DIR / "lha255e" / "lh5.lzh";
        REQUIRE(std::filesystem::exists(path));

        auto ar = lha_archive::open(path);
        REQUIRE(ar.has_value());
        verify_extract_stream(**ar, "LHA-lh5");
    }

    TEST_CASE("ZOO: extract_stream matches extract") {
        auto path = ZOO_DIR / "store.zoo";
        REQUIRE(std::filesystem::exists(path));

        auto ar = zoo_archive::open(path);
        REQUIRE(ar.has_value());
        verify_extract_stream(**ar, "ZOO-store");
    }

    TEST_CASE("RAR: extract_stream matches extract") {
        auto path = RAR_DIR / "unrar_test_01.rar";
        REQUIRE(std::filesystem::exists(path));

        auto ar = rar_archive::open(path);
        REQUIRE(ar.has_value());
        verify_extract_stream(**ar, "RAR-test01");
    }

    TEST_CASE("ACE: extract_stream matches extract") {
        auto path = ACE_DIR / "license1.ace";
        REQUIRE(std::filesystem::exists(path));

        auto ar = ace_archive::open(path);
        REQUIRE(ar.has_value());
        verify_extract_stream(**ar, "ACE-license1");
    }

    TEST_CASE("HA: extract_stream matches extract") {
        auto path = HA_DIR / "copy.ha";
        REQUIRE(std::filesystem::exists(path));

        auto ar = ha_archive::open(path);
        REQUIRE(ar.has_value());
        verify_extract_stream(**ar, "HA-copy");
    }

    TEST_CASE("HYP: extract_stream matches extract") {
        auto path = HYP_DIR / "stored.hyp";
        REQUIRE(std::filesystem::exists(path));

        auto ar = hyp_archive::open(path);
        REQUIRE(ar.has_value());
        verify_extract_stream(**ar, "HYP-stored");
    }

    TEST_CASE("CHM: extract_stream matches extract") {
        auto path = CHM_DIR / "main.chm";
        REQUIRE(std::filesystem::exists(path));

        auto ar = chm_archive::open(path);
        REQUIRE(ar.has_value());
        verify_extract_stream(**ar, "CHM-main");
    }

    TEST_CASE("Floppy: extract_stream matches extract") {
        auto path = FLOPPY_DIR / "Borland - Turbo Pascal v5.0 - Disk 1 of 3 - Install & Compiler.img";
        REQUIRE(std::filesystem::exists(path));

        auto ar = floppy_image::open(path);
        REQUIRE(ar.has_value());
        verify_extract_stream(**ar, "Floppy-disk1");
    }

    TEST_CASE("StuffIt: extract_stream matches extract") {
        auto path = STUFFIT_DIR / "test_m0.sit";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto ar = stuffit_archive::open(file);
        REQUIRE(ar.has_value());
        verify_extract_stream(**ar, "StuffIt-m0");
    }

#ifdef CRATE_WITH_LIBARCHIVE
    TEST_CASE("libarchive: extract_stream matches extract") {
        auto path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(path));

        auto ar = libarchive_archive::open(path);
        REQUIRE(ar.has_value());
        verify_extract_stream(**ar, "libarchive-zip");
    }
#endif

    TEST_CASE("extract_stream from istream-opened archive") {
        auto path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        auto archive = zip_archive::open(file);
        REQUIRE(archive.has_value());
        verify_extract_stream(**archive, "ZIP-via-istream");
    }
}

// ============================================================================
// make_*_istream factories — end-to-end decompression tests
// ============================================================================

TEST_SUITE("Decompressor istream factories") {
    TEST_CASE("gzip_decompressor single-shot with large buffer") {
        auto gz_path = GZ_DIR / "LICENSE.gz";
        REQUIRE(std::filesystem::exists(gz_path));

        auto gz_data = read_file(gz_path);
        REQUIRE(!gz_data.empty());
        auto expected = read_file(LICENSE_FILE);
        REQUIRE(!expected.empty());

        gzip_decompressor dec;
        byte_vector output_buf(64 * 1024); // large enough for full output

        byte_span input(gz_data.data(), gz_data.size());
        mutable_byte_span out(output_buf.data(), output_buf.size());

        auto result = dec.decompress_some(input, out, true);
        REQUIRE(result.has_value());

        CHECK(result->bytes_written == expected.size());
        CHECK(result->finished());
    }

    TEST_CASE("make_gzip_istream decompresses LICENSE.gz") {
        auto gz_path = GZ_DIR / "LICENSE.gz";
        REQUIRE(std::filesystem::exists(gz_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(!expected.empty());

        std::ifstream gz_file(gz_path, std::ios::binary);
        REQUIRE(gz_file.good());
        auto stream = make_gzip_istream(gz_file);
        REQUIRE(stream != nullptr);

        std::string decompressed = read_all(*stream);
        CHECK(decompressed.size() == expected.size());
        CHECK(std::equal(expected.begin(), expected.end(),
                         reinterpret_cast<const byte*>(decompressed.data()),
                         reinterpret_cast<const byte*>(decompressed.data()) + decompressed.size()));
    }

    TEST_CASE("make_inflate_istream factory creates valid stream") {
        std::istringstream empty_stream("");
        auto stream = make_inflate_istream(empty_stream);
        REQUIRE(stream != nullptr);
    }

    TEST_CASE("make_zlib_istream factory creates valid stream") {
        std::istringstream empty_stream("");
        auto stream = make_zlib_istream(empty_stream);
        REQUIRE(stream != nullptr);
    }

#ifdef CRATE_WITH_BZIP2
    TEST_CASE("make_bzip2_istream decompresses LICENSE.bz2") {
        auto bz2_path = BZ2_DIR / "LICENSE.bz2";
        REQUIRE(std::filesystem::exists(bz2_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(!expected.empty());

        std::ifstream bz2_file(bz2_path, std::ios::binary);
        REQUIRE(bz2_file.good());
        auto stream = make_bzip2_istream(bz2_file);
        REQUIRE(stream != nullptr);

        std::string decompressed = read_all(*stream);
        CHECK(decompressed.size() == expected.size());
        CHECK(std::equal(expected.begin(), expected.end(),
                         reinterpret_cast<const byte*>(decompressed.data()),
                         reinterpret_cast<const byte*>(decompressed.data()) + decompressed.size()));
    }
#endif

#ifdef CRATE_WITH_XZ
    TEST_CASE("make_xz_istream decompresses LICENSE.xz") {
        auto xz_path = XZ_DIR / "LICENSE.xz";
        REQUIRE(std::filesystem::exists(xz_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(!expected.empty());

        std::ifstream xz_file(xz_path, std::ios::binary);
        REQUIRE(xz_file.good());
        auto stream = make_xz_istream(xz_file);
        REQUIRE(stream != nullptr);

        std::string decompressed = read_all(*stream);
        CHECK(decompressed.size() == expected.size());
        CHECK(std::equal(expected.begin(), expected.end(),
                         reinterpret_cast<const byte*>(decompressed.data()),
                         reinterpret_cast<const byte*>(decompressed.data()) + decompressed.size()));
    }
#endif

#ifdef CRATE_WITH_ZSTD
    TEST_CASE("make_zstd_istream decompresses LICENSE.zst") {
        auto zst_path = ZSTD_DIR / "LICENSE.zst";
        REQUIRE(std::filesystem::exists(zst_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(!expected.empty());

        std::ifstream zst_file(zst_path, std::ios::binary);
        REQUIRE(zst_file.good());
        auto stream = make_zstd_istream(zst_file);
        REQUIRE(stream != nullptr);

        std::string decompressed = read_all(*stream);
        CHECK(decompressed.size() == expected.size());
        CHECK(std::equal(expected.begin(), expected.end(),
                         reinterpret_cast<const byte*>(decompressed.data()),
                         reinterpret_cast<const byte*>(decompressed.data()) + decompressed.size()));
    }
#endif

#ifdef CRATE_WITH_BROTLI
    TEST_CASE("make_brotli_istream decompresses LICENSE.br") {
        auto br_path = BROTLI_DIR / "LICENSE.br";
        REQUIRE(std::filesystem::exists(br_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(!expected.empty());

        std::ifstream br_file(br_path, std::ios::binary);
        REQUIRE(br_file.good());
        auto stream = make_brotli_istream(br_file);
        REQUIRE(stream != nullptr);

        std::string decompressed = read_all(*stream);
        CHECK(decompressed.size() == expected.size());
        CHECK(std::equal(expected.begin(), expected.end(),
                         reinterpret_cast<const byte*>(decompressed.data()),
                         reinterpret_cast<const byte*>(decompressed.data()) + decompressed.size()));
    }
#endif
}
