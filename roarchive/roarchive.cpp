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

#include <limits>

#include <boost/iostreams/copy.hpp>

#include "dbglog/dbglog.hpp"

#include "utility/magic.hpp"
#include "utility/binaryio.hpp"
#include "utility/streams.hpp"
#include "utility/uri.hpp"

#include "roarchive.hpp"
#include "detail.hpp"
#include "error.hpp"

namespace fs = boost::filesystem;
namespace bio = boost::iostreams;

namespace roarchive {

RoArchive::dpointer
RoArchive::factory(fs::path path, OpenOptions openOptions)
{
    if (openOptions.inlineHint) {
        // check for inline hint
        const auto str(path.string());
        auto split(str.find(openOptions.inlineHint));
        if (split != std::string::npos) {
            // found, update path and replace any hint with path suffix
            path = str.substr(0, split);
            openOptions.hint = str.substr(split + 1);
        }
    }

    if (openOptions.mime.empty()) {
        // special handling for URL
        try {
            utility::Uri uri(path.string());
            if ((uri.scheme() == "http") ||(uri.scheme() == "https")) {
                openOptions.mime = "http";
            }
        } catch (...) {}
    }

    // detect MIME type if not provided ahead
    const auto magic(openOptions.mime.empty()
                     ? utility::Magic().mime(path)
                     : openOptions.mime);

    if (magic == "inode/directory") { return directory(path, openOptions); }
    if (magic == "application/x-tar") { return tarball(path, openOptions); }
    if (magic == "application/zip") { return zip(path, openOptions); }
#ifdef ROARCHIVE_HAS_HTTP
    if (magic == "http") { return http(path, openOptions); }
#endif

    LOGTHROW(err2, NotAnArchive)
        << "Unsupported archive type <" << magic << ">.";
    return {};
}

RoArchive::RoArchive(const fs::path &path)
    : detail_(factory(path, {}))
    , directio_(detail_->directio())
{
}

RoArchive::RoArchive(const fs::path &path
                     , const OpenOptions &openOptions)
    : detail_(factory(path, openOptions))
    , directio_(detail_->directio())
{
}

RoArchive::RoArchive(const fs::path &path, const FileHint &hint
                     , const std::string &mime)
    : detail_(factory(path, OpenOptions().setHint(hint).setMime(mime)))
    , directio_(detail_->directio())
{
}

RoArchive::RoArchive(const fs::path &path, std::size_t limit
                     , const FileHint &hint, const std::string &mime)
    : detail_(factory(path, OpenOptions().setFileLimit(limit)
                      .setHint(hint)
                      .setMime(mime)))
    , directio_(detail_->directio())
{}

IStream::pointer RoArchive::istream(const fs::path &path) const
{
    auto is(detail_->istream(path));
    // set exceptions
    is->get().exceptions(std::ios::badbit | std::ios::failbit);

    return is;
}

IStream::pointer RoArchive::istream(const fs::path &path
                                    , const IStream::FilterInit &filterInit)
    const
{
    auto is(detail_->istream(path, filterInit));
    // set exceptions
    is->get().exceptions(std::ios::badbit | std::ios::failbit);
    return is;
}

bool RoArchive::exists(const fs::path &path) const
{
    return detail_->exists(path);
}

boost::optional<fs::path> RoArchive::findFile(const std::string &filename)
    const
{
    return detail_->findFile(filename);
}

fs::path RoArchive::path() const
{
    return detail().path();
}

fs::path RoArchive::path(const fs::path &path) const
{
    return path.is_absolute() ? path : (detail_->path() / path);
}

std::vector<char> IStream::read()
{
    auto &s(get());
    if (size_) {
        // we know the size of the file
        std::vector<char> buf;
        buf.resize(*size_);
        utility::binaryio::read(s, buf.data(), buf.size());
        return buf;
    } else if (seekable_) {
        // we can measure the file
        std::vector<char> buf;
        buf.resize(s.seekg(0, std::ios_base::end).tellg());
        s.seekg(0);
        utility::binaryio::read(s, buf.data(), buf.size());
        return buf;
    }

    // we need to use the old way
    std::ostringstream os;
    bio::copy(s, os);
    const auto &str(os.str());
    return { str.data(), str.data() + str.size() };
}

Files RoArchive::list() const
{
    return detail_->list();
}

RoArchive& RoArchive::applyHint(const FileHint &hint)
{
    detail_->applyHint(hint);
    return *this;
}

bool RoArchive::Detail::changed() const
{
    return stat_.changed
        (utility::FileStat::from(path_, std::nothrow));
}

bool RoArchive::changed() const
{
    return detail_->changed();
}


boost::optional<boost::filesystem::path> RoArchive::usedHint() const
{
    return detail_->usedHint();
}

bool RoArchive::handlesSchema(const std::string &schema) const
{
    return detail_->handlesSchema(schema);
}

void copy(const IStream::pointer &in, std::ostream &out)
{
    bio::copy(in->get(), out);
}

void copy(const IStream::pointer &in, const fs::path &out)
{
    utility::ofstreambuf of(out.string());
    copy(in, of);
    of.flush();
}

bool FileHint::Matcher::operator()(const fs::path &path)
{
    const auto &fname(path.filename());

    for (std::size_t index(0); index < bestIndex_; ++index) {
        if (fname == hint_[index]) {
            bestIndex_ = index;
            bestMatch_ = path;
            break;
        }
    }

    // we are done when first index is matched
    return !bestIndex_;
}

} // namespace roarchive
