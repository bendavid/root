#include "io_test.hxx"

#include "TFile.h"

#include "ROOT/RRawFileTFile.hxx"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>
using ROOT::Internal::RRawFileTFile;

namespace {

/**
 * A minimal RRawFile implementation that serves data from a string. It keeps a counter of the number of read calls
 * to help veryfing the buffer logic in the base class.
 */
class RRawFileMock : public RRawFile {
public:
   std::string fContent;
   unsigned fNumReadAt;
   bool fDiscourageReadAhead = false;

   RRawFileMock(const std::string &content, RRawFile::ROptions options)
     : RRawFile("", options), fContent(content), fNumReadAt(0) { }

   std::unique_ptr<RRawFile> Clone() const final {
      return std::make_unique<RRawFileMock>(fContent, fOptions);
   }

   void OpenImpl() final
   {
   }

   size_t ReadAtImpl(void *buffer, size_t nbytes, std::uint64_t offset) final
   {
      fNumReadAt++;
      if (offset > fContent.length())
         return 0;

      auto slice = fContent.substr(offset, nbytes);
      memcpy(buffer, slice.data(), slice.length());
      return slice.length();
   }

   std::uint64_t GetSizeImpl() final { return fContent.size(); }

   void SetDiscourageReadAheadImpl(bool val) final { fDiscourageReadAhead = val; }
};

} // anonymous namespace


TEST(RRawFile, Empty)
{
   FileRaii emptyGuard("test_rrawfile_empty", "");
   auto f = RRawFile::Create(emptyGuard.GetPath());
   EXPECT_FALSE(f->IsOpen());
   EXPECT_EQ(0u, f->GetSize());
   EXPECT_EQ(0u, f->GetFilePos());
   EXPECT_EQ(0u, f->Read(nullptr, 0));
   EXPECT_EQ(0u, f->ReadAt(nullptr, 0, 1));
   std::string line;
   EXPECT_FALSE(f->Readln(line));
   EXPECT_TRUE(f->IsOpen());

   RRawFile::ROptions options;
   options.fBlockSize = 0;
   f = RRawFile::Create(emptyGuard.GetPath(), options);
   EXPECT_EQ(0u, f->Read(nullptr, 0));
   EXPECT_EQ(0u, f->ReadAt(nullptr, 0, 1));
}


TEST(RRawFile, Basic)
{
   FileRaii basicGuard("test_rrawfile_basic", "foo\nbar");
   auto f = RRawFile::Create(basicGuard.GetPath());
   EXPECT_EQ(7u, f->GetSize());
   std::string line;
   EXPECT_TRUE(f->Readln(line));
   EXPECT_STREQ("foo", line.c_str());
   EXPECT_TRUE(f->Readln(line));
   EXPECT_STREQ("bar", line.c_str());
   EXPECT_FALSE(f->Readln(line));
   auto clone = f->Clone();
   // file pointer is reset by clone
   EXPECT_TRUE(clone->Readln(line));
   EXPECT_STREQ("foo", line.c_str());
   // Rinse and repeat
   EXPECT_EQ(4U, clone->GetFilePos());
   clone->Seek(0);
   EXPECT_TRUE(clone->Readln(line));
   EXPECT_STREQ("foo", line.c_str());

   auto f2 = RRawFile::Create("NoSuchFile");
   EXPECT_THROW(f2->Readln(line), std::runtime_error);

   auto f3 = RRawFile::Create(std::string("FiLE://") + basicGuard.GetPath());
   EXPECT_EQ(7u, f3->GetSize());

   EXPECT_THROW(RRawFile::Create(std::string("://") + basicGuard.GetPath()), std::runtime_error);
   EXPECT_THROW(RRawFile::Create("Communicator://Kirk"), std::runtime_error);
}


TEST(RRawFile, Remote)
{
#if defined(R__HAS_DAVIX) || defined(R__HAS_CURL)
   auto f = RRawFile::Create("http://root.cern/files/davix.test");
   std::string line;
   EXPECT_TRUE(f->Readln(line));
   EXPECT_STREQ("Hello, World", line.c_str());
#else
   EXPECT_THROW(RRawFile::Create("http://root.cern/files/davix.test"), std::runtime_error);
#endif
}


TEST(RRawFile, Readln)
{
   FileRaii linebreakGuard("test_rrawfile_linebreak", "foo\r\none\nline\r\n\r\n");
   auto f = RRawFile::Create(linebreakGuard.GetPath());
   std::string line;
   EXPECT_TRUE(f->Readln(line));
   EXPECT_STREQ("foo", line.c_str());
   EXPECT_TRUE(f->Readln(line));
   EXPECT_STREQ("one\nline", line.c_str());
   EXPECT_TRUE(f->Readln(line));
   EXPECT_TRUE(line.empty());
   EXPECT_FALSE(f->Readln(line));
}


TEST(RRawFile, ReadV)
{
   FileRaii readvGuard("test_rawfile_readv", "Hello, World");
   auto f = RRawFile::Create(readvGuard.GetPath());

   char buffer[2];
   buffer[0] = buffer[1] = 0;
   RRawFile::RIOVec iovec[2];
   iovec[0].fBuffer = &buffer[0];
   iovec[0].fOffset = 0;
   iovec[0].fSize = 1;
   iovec[1].fBuffer = &buffer[1];
   iovec[1].fOffset = 11;
   iovec[1].fSize = 2;
   f->ReadV(iovec, 2);

   EXPECT_EQ(1U, iovec[0].fOutBytes);
   EXPECT_EQ(1U, iovec[1].fOutBytes);
   EXPECT_EQ('H', buffer[0]);
   EXPECT_EQ('d', buffer[1]);
}


TEST(RRawFile, SplitUrl)
{
   EXPECT_STREQ("C:\\Data\\events.root", RRawFile::GetLocation("C:\\Data\\events.root").c_str());
   EXPECT_STREQ("///many/slashes", RRawFile::GetLocation("///many/slashes").c_str());
   EXPECT_STREQ("/many/slashes", RRawFile::GetLocation(":///many/slashes").c_str());
   EXPECT_STREQ("file", RRawFile::GetTransport("/foo").c_str());
   EXPECT_STREQ("http", RRawFile::GetTransport("http://").c_str());
   EXPECT_STREQ("", RRawFile::GetLocation("http://").c_str());
   EXPECT_STREQ("http", RRawFile::GetTransport("http://file:///bar").c_str());
}


TEST(RRawFile, ReadDirect)
{
   FileRaii directGuard("test_rrawfile_direct", "abc");
   char buffer;
   RRawFile::ROptions options;
   options.fBlockSize = 0;
   auto f = RRawFile::Create(directGuard.GetPath());
   EXPECT_EQ(0u, f->Read(&buffer, 0));
   EXPECT_EQ(1u, f->Read(&buffer, 1));
   EXPECT_EQ('a', buffer);
   EXPECT_EQ(1u, f->ReadAt(&buffer, 1, 2));
   EXPECT_EQ('c', buffer);

}


TEST(RRawFile, ReadBuffered)
{
   char buffer[8];
   RRawFile::ROptions options;
   options.fBlockSize = 2;
   std::unique_ptr<RRawFileMock> f(new RRawFileMock("abcdef", options));

   buffer[3] = '\0';
   EXPECT_EQ(3u, f->ReadAt(buffer, 3, 1));
   EXPECT_STREQ("bcd", buffer);
   EXPECT_EQ(1u, f->fNumReadAt); f->fNumReadAt = 0;

   buffer[2] = '\0';
   EXPECT_EQ(2u, f->ReadAt(buffer, 2, 2));
   EXPECT_STREQ("cd", buffer);
   EXPECT_EQ(2u, f->ReadAt(buffer, 2, 0));
   EXPECT_STREQ("ab", buffer);
   EXPECT_EQ(2u, f->ReadAt(buffer, 2, 2));
   EXPECT_STREQ("cd", buffer);
   EXPECT_EQ(2u, f->ReadAt(buffer, 2, 1));
   EXPECT_STREQ("bc", buffer);
   EXPECT_EQ(2u, f->fNumReadAt); f->fNumReadAt = 0;

   EXPECT_EQ(2u, f->ReadAt(buffer, 2, 0));
   EXPECT_STREQ("ab", buffer);
   EXPECT_EQ(1u, f->ReadAt(buffer, 1, 1));
   EXPECT_STREQ("bb", buffer);
   EXPECT_EQ(2u, f->ReadAt(buffer, 2, 1));
   EXPECT_STREQ("bc", buffer);
   EXPECT_EQ(0u, f->fNumReadAt); f->fNumReadAt = 0;
   EXPECT_EQ(2u, f->ReadAt(buffer, 2, 3));
   EXPECT_STREQ("de", buffer);
   EXPECT_EQ(1u, f->fNumReadAt); f->fNumReadAt = 0;
   EXPECT_EQ(1u, f->ReadAt(buffer, 1, 2));
   EXPECT_STREQ("ce", buffer);
   EXPECT_EQ(0u, f->fNumReadAt); f->fNumReadAt = 0;
   EXPECT_EQ(1u, f->ReadAt(buffer, 1, 1));
   EXPECT_STREQ("be", buffer);
   EXPECT_EQ(1u, f->fNumReadAt); f->fNumReadAt = 0;
}

TEST(RRawFile, SetBuffering)
{
   char buffer[3];
   RRawFile::ROptions options;
   options.fBlockSize = 2;
   std::unique_ptr<RRawFileMock> f(new RRawFileMock("abcd", options));

   buffer[2] = '\0';
   EXPECT_EQ(1u, f->ReadAt(buffer, 1, 0));
   EXPECT_EQ(1u, f->ReadAt(buffer + 1, 1, 1));
   EXPECT_STREQ("ab", buffer);
   EXPECT_EQ(1u, f->fNumReadAt);
   f->fNumReadAt = 0;

   f->SetBuffering(false);
   // idempotent
   f->SetBuffering(false);
   EXPECT_EQ(1u, f->ReadAt(buffer, 1, 0));
   EXPECT_EQ(1u, f->ReadAt(buffer + 1, 1, 1));
   EXPECT_STREQ("ab", buffer);
   EXPECT_EQ(2u, f->fNumReadAt);
   f->fNumReadAt = 0;

   f->SetBuffering(true);
   // idempotent
   f->SetBuffering(true);
   EXPECT_EQ(1u, f->ReadAt(buffer, 1, 2));
   EXPECT_EQ(1u, f->ReadAt(buffer + 1, 1, 3));
   EXPECT_STREQ("cd", buffer);
   EXPECT_EQ(1u, f->fNumReadAt);
   f->fNumReadAt = 0;
}

TEST(RRawFileTFile, TFile)
{
   FileRaii tfileGuard("test_rawfile_tfile.root", "");

   std::unique_ptr<TFile> file(TFile::Open(tfileGuard.GetPath().c_str(), "RECREATE"));
   file->Write();

   auto rawFile = std::make_unique<RRawFileTFile>(file.get());

   // The first four bytes should be 'root'.
   char root[5] = {};
   rawFile->ReadAt(root, 4, 0);
   EXPECT_STREQ(root, "root");

   // fBEGIN = 100, and its seek key should be 100.
   unsigned char seek[4] = {};
   rawFile->ReadAt(seek, 4, 100 + 18);
   EXPECT_EQ(seek[0], 0);
   EXPECT_EQ(seek[1], 0);
   EXPECT_EQ(seek[2], 0);
   EXPECT_EQ(seek[3], 100);
}

TEST(RRawFile, Readahead)
{
   {
      RRawFileMock mock("", ROOT::Internal::RRawFile::ROptions());
      (void)mock.GetSize();
      EXPECT_FALSE(mock.fDiscourageReadAhead);
   }

   {
      RRawFileMock mock("", ROOT::Internal::RRawFile::ROptions());
      mock.SetBuffering(false);
      EXPECT_FALSE(mock.fDiscourageReadAhead);
      (void)mock.GetSize();
      EXPECT_TRUE(mock.fDiscourageReadAhead);
      mock.SetBuffering(true);
      EXPECT_FALSE(mock.fDiscourageReadAhead);
   }
}

TEST(RRawFile, ReadVBypassesBuffer)
{
   // Vector reads are served directly, also if buffering is on: they neither read full blocks nor fill the buffer
   RRawFile::ROptions options;
   options.fBlockSize = 4;
   RRawFileMock mock("abcdefghijklmnop", options);
   ASSERT_TRUE(mock.IsBuffering());

   char buf1[2];
   char buf2[2];
   RRawFile::RIOVec iovec[2];
   iovec[0] = {buf1, 1, 2, 0};
   iovec[1] = {buf2, 10, 2, 0};
   mock.ReadV(iovec, 2);
   EXPECT_EQ(2u, mock.fNumReadAt);
   EXPECT_EQ(2u, iovec[0].fOutBytes);
   EXPECT_EQ(2u, iovec[1].fOutBytes);
   EXPECT_EQ(std::string("bc"), std::string(buf1, 2));
   EXPECT_EQ(std::string("kl"), std::string(buf2, 2));

   // The block buffer was not filled by the vector read
   char c;
   EXPECT_EQ(1u, mock.ReadAt(&c, 1, 1));
   EXPECT_EQ('b', c);
   EXPECT_EQ(3u, mock.fNumReadAt);
}

TEST(RRawFile, ShallowClone)
{
   std::string content;
   for (int i = 0; i < 4096; ++i)
      content.push_back(static_cast<char>('a' + (i * 7) % 26));
   FileRaii guard("test_rrawfile_shallowclone", content);

   // A clone of a file that is not open yet is an independent, not yet open file
   auto f = RRawFile::Create(guard.GetPath());
   auto unopened = f->ShallowClone();
   EXPECT_FALSE(unopened->IsOpen());
   EXPECT_EQ(content.size(), unopened->GetSize());

   f->SetBuffering(false);
   EXPECT_EQ(content.size(), f->GetSize());
   ASSERT_TRUE(f->IsOpen());

   // A clone of an open file shares the open file handle: it is open already and keeps the buffering mode
   auto clone = f->ShallowClone();
   EXPECT_TRUE(clone->IsOpen());
   EXPECT_FALSE(clone->IsBuffering());
   EXPECT_EQ(content.size(), clone->GetSize());
   EXPECT_EQ(0u, clone->GetFilePos());

   // The clone remains valid after the original is gone
   f.reset();
   char buf[16];
   EXPECT_EQ(16u, clone->ReadAt(buf, 16, 100));
   EXPECT_EQ(content.substr(100, 16), std::string(buf, 16));

   // Concurrent reads from shallow clones of the same file
   std::vector<std::unique_ptr<RRawFile>> clones;
   for (int t = 0; t < 8; ++t)
      clones.emplace_back(clone->ShallowClone());
   std::atomic<int> nErrors{0};
   std::vector<std::thread> threads;
   for (int t = 0; t < 8; ++t) {
      threads.emplace_back([&, t]() {
         auto &file = *clones[t];
         for (int i = 0; i < 200; ++i) {
            const std::uint64_t offset = (t * 509 + i * 131) % (content.size() - 64);
            char b1[32], b2[32];
            if (i % 2 == 0) {
               if (file.ReadAt(b1, 32, offset) != 32 || content.compare(offset, 32, b1, 32) != 0)
                  nErrors++;
            } else {
               RRawFile::RIOVec iovec[2];
               iovec[0] = {b1, offset, 32, 0};
               iovec[1] = {b2, offset + 32, 32, 0};
               file.ReadV(iovec, 2);
               if (iovec[0].fOutBytes != 32 || iovec[1].fOutBytes != 32 || content.compare(offset, 32, b1, 32) != 0 ||
                   content.compare(offset + 32, 32, b2, 32) != 0)
                  nErrors++;
            }
         }
      });
   }
   for (auto &t : threads)
      t.join();
   EXPECT_EQ(0, nErrors.load());
}
