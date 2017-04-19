/**
 * Copyright (c) 2017 Melown Technologies SE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * *  Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <map>

#include "dbglog/dbglog.hpp"

#include "utility/tar.hpp"
#include "utility/path.hpp"
#include "utility/substream.hpp"
#include "utility/streams.hpp"

#include "./detail.hpp"

namespace fs = boost::filesystem;

namespace roarchive {

namespace {

class TarIStream : public IStream {
public:
    typedef utility::io::SubStreamDevice::Filedes Filedes;

    TarIStream(const fs::path &path, const Filedes &fd)
        : path_(path), size_(fd.end - fd.start)
        , buffer_(path, fd), stream_(&buffer_)
    {
        stream_.exceptions(std::ios::badbit | std::ios::failbit);
        buf_.reset(new char[1 << 16]);
        buffer_.pubsetbuf(buf_.get(), 1 << 16);
    }

    virtual std::istream& get() { return stream_; }
    virtual fs::path path() const { return path_; }
    virtual void close() {}
    virtual boost::optional<std::size_t> size() const { return size_; }

private:
    const fs::path path_;
    const std::size_t size_;

    std::unique_ptr<char[]> buf_;
    boost::iostreams::stream_buffer<utility::io::SubStreamDevice> buffer_;
    std::istream stream_;
};

boost::filesystem::path
findPrefix(const fs::path &path, const std::string &hint
           , const utility::tar::Reader::File::list &files)
{
    for (const auto &file : files) {
        if (file.path.filename() == hint) {
            return file.path.parent_path();
        }
    }

    LOGTHROW(err2, std::runtime_error)
        << "No \"" << hint << "\" found in the tarball archive at "
        << path << ".";
    throw;
}

class TarIndex {
public:
    typedef utility::io::SubStreamDevice::Filedes Filedes;

    TarIndex(utility::tar::Reader &reader
             , const boost::optional<std::string> &hint)
        : path_(reader.path())
    {
        const auto files(reader.files());
        const auto prefix(hint ? findPrefix(path_, *hint, files) : fs::path());
        const auto fd(reader.filedes());

        for (const auto &file : files) {
            if (!utility::isPathPrefix(file.path, prefix)) { continue; }

            const auto path(utility::cutPathPrefix(file.path, prefix));
            index_.insert(map::value_type
                              (path.string()
                               , { fd, file.start, file.end() }));
        }
    }

    const Filedes& file(const std::string &path) const {
        auto findex(index_.find(path));
        if (findex == index_.end()) {
            LOGTHROW(err2, std::runtime_error)
                << "File \"" << path << "\" not found in the archive at "
                << path_ << ".";
        }
        return findex->second;
    }

private:
    const fs::path path_;
    typedef std::map<std::string, Filedes> map;
    map index_;
};

class Tarball : public RoArchive::Detail {
public:
    Tarball(const boost::filesystem::path &path
            , const boost::optional<std::string> &hint)
        : Detail(path), reader_(path), index_(reader_, hint)
    {}

    /** Get (wrapped) input stream for given file.
     *  Throws when not found.
     */
    virtual IStream::pointer istream(const boost::filesystem::path &path) const
    {
        return std::make_shared<TarIStream>(path, index_.file(path.string()));
    }

private:
    utility::tar::Reader reader_;
    TarIndex index_;
};

} // namespace

RoArchive::dpointer
RoArchive::tarball(const boost::filesystem::path &path
                   , const boost::optional<std::string> &hint)
{
    return std::make_shared<Tarball>(path, hint);
}

} // namespace roarchive
