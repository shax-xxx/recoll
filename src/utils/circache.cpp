/* Copyright (C) 2009 J.F.Dockes
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "autoconfig.h"

#include "circache.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "safefcntl.h"
#include <sys/types.h>
#include "safesysstat.h"
#include "safeunistd.h"
#include <assert.h>
#include <memory.h>
#include <inttypes.h>
#include <memory>
#include <functional>
#include <utility>
#include <sstream>
#include <iostream>
#include <map>

#ifndef _WIN32
#include <sys/uio.h>
#else
struct iovec {
    void *iov_base;
    size_t iov_len;
};
static ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t tot = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t ret = ::write(fd, iov[i].iov_base, iov[i].iov_len);
        if (ret > 0) {
            tot += ret;
        }
        if (ret != (ssize_t)iov[i].iov_len) {
            return ret == -1 ? -1 : tot;
        }
    }
    return tot;
}
#endif

#include "chrono.h"
#include "conftree.h"
#include "copyfile.h"
#include "cstr.h"
#include "log.h"
#include "md5.h"
#include "pathut.h"
#include "smallut.h"
#include "wipedir.h"
#include "zlibut.h"

using namespace std;
using namespace std::placeholders;

/*
 * File structure:
 * - Starts with a 1-KB header block, with a param dictionary.
 * - Stored items follow. Each item has a header and 2 segments for
 *   the metadata and the data.
 *   The segment sizes are stored in the ascii header/marker:
 *     circacheSizes = xxx yyy zzz
 *     xxx bytes of metadata
 *     yyy bytes of data
 *     zzz bytes of padding up to next object (only one entry has non zero)
 *
 * There is a write position, which can be at eof while the file is growing, or inside the file if
 * we are recycling. This is stored in the header (oheadoffs), together with the maximum size. See
 * the Internal class for more detailed comments.
 *
 * If we are recycling, we have to take care to compute the size of the
 * possible remaining area from the last object invalidated by the write,
 * pad it with neutral data and store the size in the new header. To help with
 * this, the address for the last object written is also kept in the header
 * (nheadoffs, npadsize)
 */

// First block size
#define CIRCACHE_FIRSTBLOCK_SIZE 1024

// Entry header.
// 2x32 1x64 bits ints as hex integers + 1 x 16 bits flag + at least 1 zero
//                          15 +             2x9 + 17 + 3 + 1 = 54
static const char *headerformat = "circacheSizes = %x %x %llx %hx";
#define CIRCACHE_HEADER_SIZE 64

class EntryHeaderData {
public:
    EntryHeaderData() : dicsize(0), datasize(0), padsize(0), flags(0) {}
    unsigned int dicsize;
    unsigned int datasize;
    uint64_t padsize;
    unsigned short flags;
};
enum EntryFlags {EFNone = 0, EFDataCompressed = 1};

// A callback class for the header-hopping function.
class CCScanHook {
public:
    virtual ~CCScanHook() {}
    enum status {Stop, Continue, Error, Eof};
    virtual status takeone(int64_t offs, const string& udi,
                           const EntryHeaderData& d) = 0;
};

// We have an auxiliary in-memory multimap of hashed-udi -> offset to
// speed things up. This is created the first time the file is scanned
// (on the first get), and not saved to disk.

// The map key: hashed udi. As a very short hash seems sufficient,
// maybe we could find something faster/simpler than md5?
#define UDIHLEN 4
class UdiH {
public:
    unsigned char h[UDIHLEN];

    UdiH(const string& udi) {
        MD5_CTX ctx;
        MD5Init(&ctx);
        MD5Update(&ctx, (const unsigned char*)udi.c_str(), udi.length());
        unsigned char md[16];
        MD5Final(md, &ctx);
        memcpy(h, md, UDIHLEN);
    }

    string asHexString() const {
        static const char hex[] = "0123456789abcdef";
        string out;
        for (int i = 0; i < UDIHLEN; i++) {
            out.append(1, hex[h[i] >> 4]);
            out.append(1, hex[h[i] & 0x0f]);
        }
        return out;
    }
    bool operator==(const UdiH& r) const {
        for (int i = 0; i < UDIHLEN; i++)
            if (h[i] != r.h[i]) {
                return false;
            }
        return true;
    }
    bool operator<(const UdiH& r) const {
        for (int i = 0; i < UDIHLEN; i++) {
            if (h[i] < r.h[i]) {
                return true;
            }
            if (h[i] > r.h[i]) {
                return false;
            }
        }
        return false;
    }
};
typedef multimap<UdiH, int64_t> kh_type;
typedef multimap<UdiH, int64_t>::value_type kh_value_type;

class CirCacheInternal {
public:
    int m_fd;
    ////// These are cache persistent state and written to the first block:
    // Maximum file size, after which we begin reusing old space
    int64_t m_maxsize;
    // Offset of the oldest header, or max file offset (file size)
    // while the file is growing. This is the next write position.
    int64_t m_oheadoffs;
    // Offset of last write (newest header)
    int64_t m_nheadoffs;
    // Pad size for newest entry.
    int64_t m_npadsize;
    // Keep history or only last entry
    bool  m_uniquentries;
    ///////////////////// End header entries

    // A place to hold data when reading
    char  *m_buffer;
    size_t m_bufsiz;

    // Error messages
    ostringstream m_reason;

    // State for rewind/next/getcurrent operation. This could/should
    // be moved to a separate iterator.
    int64_t  m_itoffs;
    EntryHeaderData m_ithd;

    // Offset cache
    kh_type m_ofskh;
    bool    m_ofskhcplt; // Has cache been fully read since open?

    // Add udi->offset translation to map
    bool khEnter(const string& udi, int64_t ofs) {
        UdiH h(udi);

        LOGDEB2("Circache::khEnter: h " << h.asHexString() << " offs " <<
                ofs << " udi [" << udi << "]\n");

        pair<kh_type::iterator, kh_type::iterator> p = m_ofskh.equal_range(h);

        if (p.first != m_ofskh.end() && p.first->first == h) {
            for (kh_type::iterator it = p.first; it != p.second; it++) {
                LOGDEB2("Circache::khEnter: col h " << it->first.asHexString() <<
                        ", ofs " << it->second << "\n");
                if (it->second == ofs) {
                    // (h,offs) already there. Happens
                    LOGDEB2("Circache::khEnter: already there\n");
                    return true;
                }
            }
        }
        m_ofskh.insert(kh_value_type(h, ofs));
        LOGDEB2("Circache::khEnter: inserted\n");
        return true;
    }
    void khDump() {
        for (const auto& e : m_ofskh) {
            LOGDEB("Circache::KHDUMP: " << e.first.asHexString() << " " << e.second << "\n");
        }
    }

    // Return vector of candidate offsets for udi (possibly several
    // because there may be hash collisions, and also multiple
    // instances).
    bool khFind(const string& udi, vector<int64_t>& ofss) {
        ofss.clear();

        UdiH h(udi);

        LOGDEB2("Circache::khFind: h " << h.asHexString() << " udi [" << udi << "]\n");

        pair<kh_type::iterator, kh_type::iterator> p = m_ofskh.equal_range(h);

#if 0
        if (p.first == m_ofskh.end()) {
            LOGDEB("KHFIND: FIRST END()\n");
        }
        if (p.second == m_ofskh.end()) {
            LOGDEB("KHFIND: SECOND END()\n");
        }
        if (!(p.first->first == h))
            LOGDEB("KHFIND: NOKEY: " << p.first->first.asHexString() << " " <<
                   p.second->first.asHexString() << "\n");
#endif

        if (p.first == m_ofskh.end() || !(p.first->first == h)) {
            return false;
        }

        for (kh_type::iterator it = p.first; it != p.second; it++) {
            ofss.push_back(it->second);
        }
        return true;
    }
    // Clear entry for udi/offs
    bool khClear(const pair<string, int64_t>& ref) {
        UdiH h(ref.first);
        pair<kh_type::iterator, kh_type::iterator> p = m_ofskh.equal_range(h);
        if (p.first != m_ofskh.end() && (p.first->first == h)) {
            for (kh_type::iterator it = p.first; it != p.second;) {
                kh_type::iterator tmp = it++;
                if (tmp->second == ref.second) {
                    m_ofskh.erase(tmp);
                }
            }
        }
        return true;
    }
    // Clear entries for vector of udi/offs
    bool khClear(const vector<pair<string, int64_t> >& udis) {
        for (const auto& udioffs : udis) {
            khClear(udioffs);
        }
        return true;
    }
    // Clear all entries for udi
    bool khClear(const string& udi) {
        UdiH h(udi);
        pair<kh_type::iterator, kh_type::iterator> p = m_ofskh.equal_range(h);
        if (p.first != m_ofskh.end() && (p.first->first == h)) {
            for (kh_type::iterator it = p.first; it != p.second;) {
                kh_type::iterator tmp = it++;
                m_ofskh.erase(tmp);
            }
        }
        return true;
    }
    CirCacheInternal()
        : m_fd(-1), m_maxsize(-1), m_oheadoffs(-1),
          m_nheadoffs(0), m_npadsize(0), m_uniquentries(false),
          m_buffer(nullptr), m_bufsiz(0), m_ofskhcplt(false) {
    }

    ~CirCacheInternal() {
        if (m_fd >= 0) {
            close(m_fd);
        }
        if (m_buffer) {
            free(m_buffer);
        }
    }

    char *buf(size_t sz) {
        if (m_bufsiz >= sz) {
            return m_buffer;
        }
        if ((m_buffer = (char *)realloc(m_buffer, sz))) {
            m_bufsiz = sz;
        } else {
            m_reason << "CirCache:: realloc(" << sz << ") failed";
            m_bufsiz = 0;
        }
        return m_buffer;
    }

    // Name for the cache file
    string datafn(const string& d) {
        return  path_cat(d, "circache.crch");
    }

    bool writefirstblock() {
        if (m_fd < 0) {
            m_reason << "writefirstblock: not open ";
            return false;
        }

        ostringstream s;
        s <<
            "maxsize = " << m_maxsize << "\n" <<
            "oheadoffs = " << m_oheadoffs << "\n" <<
            "nheadoffs = " << m_nheadoffs << "\n" <<
            "npadsize = " << m_npadsize   << "\n" <<
            "unient = " << m_uniquentries << "\n" <<
            "                                                              " <<
            "                                                              " <<
            "                                                              " <<
            "\0";

        int sz = int(s.str().size());
        assert(sz < CIRCACHE_FIRSTBLOCK_SIZE);
        lseek(m_fd, 0, 0);
        if (write(m_fd, s.str().c_str(), sz) != sz) {
            m_reason << "writefirstblock: write() failed: errno " << errno;
            return false;
        }
        return true;
    }

    bool readfirstblock() {
        if (m_fd < 0) {
            m_reason << "readfirstblock: not open ";
            return false;
        }

        char bf[CIRCACHE_FIRSTBLOCK_SIZE];

        lseek(m_fd, 0, 0);
        if (read(m_fd, bf, CIRCACHE_FIRSTBLOCK_SIZE) != CIRCACHE_FIRSTBLOCK_SIZE) {
            m_reason << "readfirstblock: read() failed: errno " << errno;
            return false;
        }
        string s(bf, CIRCACHE_FIRSTBLOCK_SIZE);
        ConfSimple conf(s, 1);
        m_maxsize = conf.getInt("maxsize", -1);
        if (m_maxsize == -1) {
            m_reason << "readfirstblock: conf get maxsize failed";
            return false;
        }
        m_oheadoffs = conf.getInt("oheadoffs", -1);
        if (m_oheadoffs == -1) {
            m_reason << "readfirstblock: conf get oheadoffs failed";
            return false;
        }
        m_nheadoffs = conf.getInt("nheadoffs", -1);
        if (m_nheadoffs == -1) {
            m_reason << "readfirstblock: conf get nheadoffs failed";
            return false;
        }
        m_npadsize = conf.getInt("npadsize", -1);
        if (m_npadsize == -1) {
            m_reason << "readfirstblock: conf get npadsize failed";
            return false;
        }
        m_uniquentries = conf.getBool("unient", false);
        return true;
    }

    bool writeEntryHeader(int64_t offset, const EntryHeaderData& d, bool eraseData = false) {
        if (m_fd < 0) {
            m_reason << "writeEntryHeader: not open ";
            return false;
        }
        char bf[CIRCACHE_HEADER_SIZE];
        memset(bf, 0, CIRCACHE_HEADER_SIZE);
        snprintf(bf, CIRCACHE_HEADER_SIZE, headerformat, d.dicsize, d.datasize, d.padsize, d.flags);
        if (lseek(m_fd, offset, 0) != offset) {
            m_reason << "CirCache::weh: lseek(" << offset << ") failed: errno " << errno;
            return false;
        }
        if (write(m_fd, bf, CIRCACHE_HEADER_SIZE) !=  CIRCACHE_HEADER_SIZE) {
            m_reason << "CirCache::weh: write failed. errno " << errno;
            return false;
        }
        if (eraseData == true) {
            if (d.dicsize || d.datasize) {
                m_reason << "CirCache::weh: erase requested but not empty";
                return false;
            }
            string buf((size_t)d.padsize, ' ');
            if (write(m_fd, buf.c_str(), (size_t)d.padsize) != (ssize_t)d.padsize) {
                m_reason << "CirCache::weh: write failed. errno " << errno;
                return false;
            }
        }
        return true;
    }

    CCScanHook::status readEntryHeader(int64_t offset, EntryHeaderData& d) {
        if (m_fd < 0) {
            m_reason << "readEntryHeader: not open ";
            return CCScanHook::Error;
        }

        if (lseek(m_fd, offset, 0) != offset) {
            m_reason << "readEntryHeader: lseek(" << offset <<
                ") failed: errno " << errno;
            return CCScanHook::Error;
        }
        char bf[CIRCACHE_HEADER_SIZE];

        int ret = read(m_fd, bf, CIRCACHE_HEADER_SIZE);
        if (ret == 0) {
            // Eof
            m_reason << " Eof ";
            return CCScanHook::Eof;
        }
        if (ret != CIRCACHE_HEADER_SIZE) {
            m_reason << " readheader: read failed errno " << errno;
            return CCScanHook::Error;
        }
        if (sscanf(bf, headerformat, &d.dicsize, &d.datasize,
                   &d.padsize, &d.flags) != 4) {
            m_reason << " readEntryHeader: bad header at " <<
                offset << " [" << bf << "]";
            return CCScanHook::Error;
        }
        LOGDEB2("Circache:readEntryHeader: dcsz " << d.dicsize << " dtsz " <<
                d.datasize << " pdsz " << d.padsize << " flgs " <<d.flags<<"\n");
        return CCScanHook::Continue;
    }

    CCScanHook::status scan(int64_t startoffset, CCScanHook *user, bool fold = false) {
        if (m_fd < 0) {
            m_reason << "scan: not open ";
            return CCScanHook::Error;
        }

        int64_t so0 = startoffset;
        bool already_folded = false;

        while (true) {
            if (already_folded && startoffset == so0) {
                m_ofskhcplt = true;
                return CCScanHook::Eof;
            }

            EntryHeaderData d;
            CCScanHook::status st;
            switch ((st = readEntryHeader(startoffset, d))) {
            case CCScanHook::Continue:
                break;
            case CCScanHook::Eof:
                if (fold && !already_folded) {
                    already_folded = true;
                    startoffset = CIRCACHE_FIRSTBLOCK_SIZE;
                    continue;
                }
                /* FALLTHROUGH */
            default:
                return st;
            }

            string udi;
            if (d.dicsize) {
                // d.dicsize is 0 for erased entries
                char *bf;
                if ((bf = buf(d.dicsize + 1)) == nullptr) {
                    return CCScanHook::Error;
                }
                bf[d.dicsize] = 0;
                if (read(m_fd, bf, d.dicsize) != int(d.dicsize)) {
                    m_reason << "scan: read failed errno " << errno;
                    return CCScanHook::Error;
                }
                string b(bf, d.dicsize);
                ConfSimple conf(b, 1);

                if (!conf.get("udi", udi, cstr_null)) {
                    m_reason << "scan: no udi in dic";
                    return CCScanHook::Error;
                }
                khEnter(udi, startoffset);
            }

            // Call callback
            CCScanHook::status a =
                user->takeone(startoffset, udi, d);
            switch (a) {
            case CCScanHook::Continue:
                break;
            default:
                return a;
            }

            startoffset += CIRCACHE_HEADER_SIZE + d.dicsize +
                d.datasize + d.padsize;
        }
    }

    bool readHUdi(int64_t hoffs, EntryHeaderData& d, string& udi) {
        if (readEntryHeader(hoffs, d) != CCScanHook::Continue) {
            return false;
        }
        string dic;
        if (!readDicData(hoffs, d, dic, nullptr)) {
            return false;
        }
        if (d.dicsize == 0) {
            // This is an erased entry
            udi.erase();
            return true;
        }
        ConfSimple conf(dic);
        if (!conf.get("udi", udi)) {
            m_reason << "Bad file: no udi in dic";
            return false;
        }
        return true;
    }

    bool readDicData(int64_t hoffs, EntryHeaderData& hd, string& dic, string* data) {
        int64_t offs = hoffs + CIRCACHE_HEADER_SIZE;
        // This syscall could be avoided in some cases if we saved the offset
        // at each seek. In most cases, we just read the header and we are
        // at the right position
        if (lseek(m_fd, offs, 0) != offs) {
            m_reason << "CirCache::get: lseek(" << offs << ") failed: " << errno;
            return false;
        }
        char *bf = nullptr;
        if (hd.dicsize) {
            bf = buf(hd.dicsize);
            if (nullptr == bf) {
                return false;
            }
            if (read(m_fd, bf, hd.dicsize) != int(hd.dicsize)) {
                m_reason << "CirCache::get: read() failed: errno " << errno;
                return false;
            }
            dic.assign(bf, hd.dicsize);
        } else {
            dic.erase();
        }
        if (nullptr == data) {
            return true;
        }

        if (hd.datasize) {
            bf = buf(hd.datasize);
            if (nullptr == bf) {
                return false;
            }
            if (read(m_fd, bf, hd.datasize) != int(hd.datasize)) {
                m_reason << "CirCache::get: read() failed: errno " << errno;
                return false;
            }

            if (hd.flags & EFDataCompressed) {
                LOGDEB1("Circache:readdicdata: data compressed\n");
                ZLibUtBuf buf;
                if (!inflateToBuf(bf, hd.datasize, buf)) {
                    m_reason << "CirCache: decompression failed ";
                    return false;
                }
                data->assign(buf.getBuf(), buf.getCnt());
            } else {
                LOGDEB1("Circache:readdicdata: data NOT compressed\n");
                data->assign(bf, hd.datasize);
            }
        } else {
            data->erase();
        }
        return true;
    }

};

CirCache::CirCache(const string& dir)
    : m_dir(dir)
{
    m_d = new CirCacheInternal;
    LOGDEB0("CirCache: [" << m_dir << "]\n");
}

CirCache::~CirCache()
{
    delete m_d;
    m_d = nullptr;
}

string CirCache::getReason()
{
    return m_d ? m_d->m_reason.str() : "Not initialized";
}

// A scan callback which just records the last header offset and
// padsize seen. This is used with a scan(nofold) to find the last
// physical record in the file
class CCScanHookRecord : public  CCScanHook {
public:
    int64_t headoffs;
    int64_t padsize;
    CCScanHookRecord()
        : headoffs(0), padsize(0) {
    }
    virtual status takeone(int64_t offs, const string&,
                           const EntryHeaderData& d) {
        headoffs = offs;
        padsize = d.padsize;
        LOGDEB2("CCScanHookRecord::takeone: offs " << headoffs << " padsize " <<
                padsize << "\n");
        return Continue;
    }
};

string CirCache::getpath() const
{
    return m_d->datafn(m_dir);
}

bool CirCache::create(int64_t maxsize, int flags)
{
    LOGDEB("CirCache::create: [" << m_dir << "] maxsz " << maxsize <<
           " flags 0x" << std::hex << flags <<std::dec<<"\n");
    if (nullptr == m_d) {
        LOGERR("CirCache::create: null data\n");
        return false;
    }

    struct stat st;
    if (stat(m_dir.c_str(), &st) < 0) {
        // Directory does not exist, create it
        if (!path_makepath(m_dir, 0777)) {
            m_d->m_reason << "CirCache::create: mkdir(" << m_dir << ") failed. errno: " << errno;
            return false;
        }
    } else {
        // If the file exists too, and truncate is not set, switch
        // to open-mode. Still may need to update header params.
        if (path_exists(m_d->datafn(m_dir)) && !(flags & CC_CRTRUNCATE)) {
            if (!this->open(CC_OPWRITE)) {
                return false;
            }
            if (maxsize == m_d->m_maxsize &&
                ((flags & CC_CRUNIQUE) != 0) == m_d->m_uniquentries) {
                LOGDEB("Header unchanged, no rewrite\n");
                return true;
            }
            // If the new maxsize is bigger than current size, we need
            // to stop recycling if this is what we are doing.
            if (maxsize > m_d->m_maxsize && maxsize > st.st_size) {
                // Scan the file to find the last physical record. The
                // ohead is set at physical eof, and nhead is the last
                // scanned record
                CCScanHookRecord rec;
                m_d->scan(CIRCACHE_FIRSTBLOCK_SIZE, &rec, false);
                m_d->m_oheadoffs = lseek(m_d->m_fd, 0, SEEK_END);
                m_d->m_nheadoffs = rec.headoffs;
                m_d->m_npadsize = rec.padsize;
            }
            m_d->m_maxsize = maxsize;
            m_d->m_uniquentries = ((flags & CC_CRUNIQUE) != 0);
            LOGDEB2("CirCache::create: rewriting header with maxsize " <<
                    m_d->m_maxsize << " oheadoffs " << m_d->m_oheadoffs <<
                    " nheadoffs " << m_d->m_nheadoffs << " npadsize " <<
                    m_d->m_npadsize << " unient " << m_d->m_uniquentries <<"\n");
            return m_d->writefirstblock();
        }
        // Else fallthrough to create file
    }

    if ((m_d->m_fd = path_open(m_d->datafn(m_dir),
                               O_CREAT | O_RDWR | O_TRUNC | O_BINARY, 0666)) < 0) {
        m_d->m_reason << "CirCache::create: open/creat(" <<
            m_d->datafn(m_dir) << ") failed " << "errno " << errno;
        return false;
    }

    m_d->m_maxsize = maxsize;
    m_d->m_oheadoffs = CIRCACHE_FIRSTBLOCK_SIZE;
    m_d->m_uniquentries = ((flags & CC_CRUNIQUE) != 0);

    char buf[CIRCACHE_FIRSTBLOCK_SIZE];
    memset(buf, 0, CIRCACHE_FIRSTBLOCK_SIZE);
    if (::write(m_d->m_fd, buf, CIRCACHE_FIRSTBLOCK_SIZE) !=
        CIRCACHE_FIRSTBLOCK_SIZE) {
        m_d->m_reason << "CirCache::create: write header failed, errno "
                      << errno;
        return false;
    }
    return m_d->writefirstblock();
}

bool CirCache::open(OpMode mode)
{
    if (nullptr == m_d) {
        LOGERR("CirCache::open: null data\n");
        return false;
    }

    if (m_d->m_fd >= 0) {
        ::close(m_d->m_fd);
    }

    if ((m_d->m_fd = path_open(m_d->datafn(m_dir),
                            mode == CC_OPREAD ? O_RDONLY | O_BINARY : O_RDWR | O_BINARY)) < 0) {
        m_d->m_reason << "CirCache::open: open(" << m_d->datafn(m_dir) <<
            ") failed " << "errno " << errno;
        return false;
    }
    return m_d->readfirstblock();
}

int64_t CirCache::size() const
{
    if (nullptr == m_d) {
        LOGERR("CirCache::open: null data\n");
        return -1;
    }
    int64_t sz = -1;
    struct PathStat st;
    if (m_d->m_fd < 0) {
        if (path_fileprops(m_d->datafn(m_dir), &st) < 0) {
            m_d->m_reason << "CirCache::size: stat(" << m_d->datafn(m_dir) <<
                ") failed " << "errno " << errno;
            return -1;
        }
        sz = st.pst_size;
    } else {
        struct stat st;
        if (fstat(m_d->m_fd, &st) < 0) {
            m_d->m_reason << "CirCache::open: fstat(" << m_d->datafn(m_dir) <<
                ") failed " << "errno " << errno;
            return -1;
        }
        sz = st.st_size;
    }
    return sz;
}

int64_t CirCache::maxsize() const
{
    if (nullptr == m_d) {
        LOGERR("CirCache::open: null data\n");
        return -1;
    }
    return m_d->m_maxsize;
}

int64_t CirCache::writepos() const
{
    if (nullptr == m_d) {
        LOGERR("CirCache::open: null data\n");
        return -1;
    }
    return m_d->m_oheadoffs;
}

int64_t CirCache::nheadpos() const
{
    if (nullptr == m_d) {
        LOGERR("CirCache::open: null data\n");
        return -1;
    }
    return m_d->m_nheadoffs;
}

bool CirCache::uniquentries() const
{
    if (nullptr == m_d) {
        LOGERR("CirCache::open: null data\n");
        return false;
    }
    return m_d->m_uniquentries;
}


class CCScanHookDump : public  CCScanHook {
public:
    virtual status takeone(int64_t offs, const string& udi,
                           const EntryHeaderData& d) {
        cout << "Scan: offs " << offs << " dicsize " << d.dicsize
             << " datasize " << d.datasize << " padsize " << d.padsize <<
            " flags " << d.flags <<
            " udi [" << udi << "]" << endl;
        return Continue;
    }
};

bool CirCache::dump()
{
    CCScanHookDump dumper;

    // Start at oldest header. This is eof while the file is growing, scan will
    // fold to bot at once.
    int64_t start = m_d->m_oheadoffs;

    switch (m_d->scan(start, &dumper, true)) {
    case CCScanHook::Stop:
        cout << "Scan returns Stop??" << endl;
        return false;
    case CCScanHook::Continue:
        cout << "Scan returns Continue ?? " << CCScanHook::Continue << " " <<
            getReason() << endl;
        return false;
    case CCScanHook::Error:
        cout << "Scan returns Error: " << getReason() << endl;
        return false;
    case CCScanHook::Eof:
        cout << "Scan returns Eof (ok)" << endl;
        return true;
    default:
        cout << "Scan returns Unknown ??" << endl;
        return false;
    }
}

class CCScanHookGetter : public  CCScanHook {
public:
    string  m_udi;
    int     m_targinstance;
    int     m_instance;
    int64_t   m_offs;
    EntryHeaderData m_hd;

    CCScanHookGetter(const string& udi, int ti)
        : m_udi(udi), m_targinstance(ti), m_instance(0), m_offs(0) {}

    virtual status takeone(int64_t offs, const string& udi,
                           const EntryHeaderData& d) {
        LOGDEB1("Circache:Scan: off " << offs << " udi [" << udi << "] dcsz " <<
                d.dicsize << " dtsz " << d.datasize << " pdsz " << d.padsize <<
               " flgs " << d.flags << " previnst " << m_instance << "\n");
        if (!m_udi.compare(udi)) {
            m_instance++;
            m_offs = offs;
            m_hd = d;
            if (m_instance == m_targinstance) {
                return Stop;
            }
        }
        return Continue;
    }
};

bool CirCache::get(const string& udi, string& dic, string *data, int instance)
{
    Chrono chron;
    if (m_d->m_fd < 0) {
        m_d->m_reason << "CirCache::get: no data or not open";
        return false;
    }

    LOGDEB0("CirCache::get: udi [" << udi << "], instance " << instance << "\n");

    // If memory map is up to date, use it:
    if (m_d->m_ofskhcplt) {
        LOGDEB1("CirCache::get: using ofskh\n");
        //m_d->khDump();
        vector<int64_t> ofss;
        if (m_d->khFind(udi, ofss)) {
            LOGDEB1("Circache::get: h found, colls " << ofss.size() << "\n");
            int finst = 1;
            EntryHeaderData d_good;
            int64_t           o_good = 0;
            for (const auto& offset : ofss) {
                LOGDEB1("Circache::get: trying offs " << offset << "\n");
                EntryHeaderData d;
                string fudi;
                if (!m_d->readHUdi(offset, d, fudi)) {
                    return false;
                }
                if (!fudi.compare(udi)) {
                    // Found one, memorize offset. Done if instance
                    // matches, else go on. If instance is -1 need to
                    // go to the end anyway
                    d_good = d;
                    o_good = offset;
                    if (finst == instance) {
                        break;
                    } else {
                        finst++;
                    }
                }
            }
            // Did we read an appropriate entry ?
            if (o_good != 0 && (instance == -1 || instance == finst)) {
                bool ret = m_d->readDicData(o_good, d_good, dic, data);
                LOGDEB0("Circache::get: hfound, " << chron.millis() << " mS\n");
                return ret;
            }
            // Else try to scan anyway.
        }
    }

    CCScanHookGetter getter(udi, instance);
    int64_t start = m_d->m_oheadoffs;

    CCScanHook::status ret = m_d->scan(start, &getter, true);
    if (ret == CCScanHook::Eof) {
        if (getter.m_instance == 0) {
            return false;
        }
    } else if (ret != CCScanHook::Stop) {
        return false;
    }
    bool bret = m_d->readDicData(getter.m_offs, getter.m_hd, dic, data);
    LOGDEB0("Circache::get: scanfound, " << chron.millis() << " mS\n");
    return bret;
}

// It would be possible to have an option to only erase if this is the
// last entry in the file, by comparing the offsets from khFind() with
// m_oheadoffs. Read the last offset < m_oheadoffs and check that
// offset+sizes == oheadoffs
bool CirCache::erase(const string& udi, bool reallyclear)
{
    if (nullptr == m_d) {
        LOGERR("CirCache::erase: null data\n");
        return false;
    }
    if (m_d->m_fd < 0) {
        m_d->m_reason << "CirCache::erase: no data or not open";
        return false;
    }

    LOGDEB0("CirCache::erase: udi [" << udi << "]\n");

    // If the mem cache is not up to date, update it, we're too lazy
    // to do a scan
    if (!m_d->m_ofskhcplt) {
        string dic;
        get("nosuchudi probably exists", dic);
        if (!m_d->m_ofskhcplt) {
            LOGERR("CirCache::erase : cache not updated after get\n");
            return false;
        }
    }

    vector<int64_t> ofss;
    if (!m_d->khFind(udi, ofss)) {
        // Udi not in there,  erase ok
        LOGDEB("CirCache::erase: khFind returns none\n");
        return true;
    }

    for (const auto& offset : ofss) {
        LOGDEB2("CirCache::erase: reading at " << offset << "\n");
        EntryHeaderData d;
        string fudi;
        if (!m_d->readHUdi(offset, d, fudi)) {
            return false;
        }
        LOGDEB2("CirCache::erase: found fudi [" << fudi << "]\n");
        if (!fudi.compare(udi)) {
            EntryHeaderData nd;
            nd.padsize = d.dicsize + d.datasize + d.padsize;
            LOGDEB2("CirCache::erase: rewrite at " << offset << "\n");
            if (offset == m_d->m_nheadoffs) {
                m_d->m_npadsize = nd.padsize;
            }
            if (!m_d->writeEntryHeader(offset, nd, reallyclear)) {
                LOGERR("CirCache::erase: write header failed\n");
                return false;
            }
        }
    }
    m_d->khClear(udi);
    return true;
}

// Used to scan the file ahead until we accumulated enough space for the new
// entry.
class CCScanHookSpacer : public  CCScanHook {
public:
    int64_t sizewanted;
    int64_t sizeseen;
    vector<pair<string, int64_t> > squashed_udis;
    CCScanHookSpacer(int64_t sz)
        : sizewanted(sz), sizeseen(0) {
        assert(sz > 0);
    }

    virtual status takeone(int64_t offs, const string& udi,
                           const EntryHeaderData& d) {
        LOGDEB2("Circache:ScanSpacer:off " << offs << " dcsz " << d.dicsize <<
                " dtsz " << d.datasize <<
                " pdsz " << d.padsize << " udi[" << udi << "]\n");
        sizeseen += CIRCACHE_HEADER_SIZE + d.dicsize + d.datasize + d.padsize;
        squashed_udis.push_back(make_pair(udi, offs));
        if (sizeseen >= sizewanted) {
            return Stop;
        }
        return Continue;
    }
};

bool CirCache::put(const string& udi, const ConfSimple *iconf,
                   const string& data, unsigned int iflags)
{
    if (nullptr == m_d) {
        LOGERR("CirCache::put: null data\n");
        return false;
    }
    if (m_d->m_fd < 0) {
        m_d->m_reason << "CirCache::put: no data or not open";
        return false;
    }

    // We need the udi in input metadata
    string dic;
    if (!iconf || !iconf->get("udi", dic) || dic.empty() || dic.compare(udi)) {
        m_d->m_reason << "No/bad 'udi' entry in input dic";
        LOGERR("Circache::put: no/bad udi: DIC:[" << dic << "] UDI [" << udi <<
               "]\n");
        return false;
    }

    // Possibly erase older entries. Need to do this first because we may be
    // able to reuse the space if the same udi was last written
    if (m_d->m_uniquentries && !erase(udi)) {
        LOGERR("CirCache::put: can't erase older entries\n");
        return false;
    }

    ostringstream s;
    iconf->write(s);
    dic = s.str();

    // Data compression ?
    const char *datap = data.c_str();
    size_t datalen = data.size();
    unsigned short flags = 0;
    ZLibUtBuf buf;
    if (!(iflags & NoCompHint)) {
        if (deflateToBuf(data.c_str(), data.size(), buf)) {
            // If compression succeeds, and the ratio makes sense, store compressed
            if (float(buf.getCnt()) < 0.9 * float(data.size())) {
                datap = buf.getBuf();
                datalen = buf.getCnt();
                flags |= EFDataCompressed;
            }
        }
    }

    struct stat st;
    if (fstat(m_d->m_fd, &st) < 0) {
        m_d->m_reason << "CirCache::put: fstat failed. errno " << errno;
        return false;
    }

    // Characteristics for the new entry.
    int64_t nsize = CIRCACHE_HEADER_SIZE + dic.size() + datalen;
    int64_t nwriteoffs = m_d->m_oheadoffs;
    int64_t npadsize = 0;
    bool extending = false;

    LOGDEB("CirCache::put: nsz " << nsize << " oheadoffs " <<
           m_d->m_oheadoffs << "\n");

    // Check if we can recover some pad space from the (physically) previous
    // entry.
    int64_t recovpadsize = m_d->m_oheadoffs == CIRCACHE_FIRSTBLOCK_SIZE ?
        0 : m_d->m_npadsize;
    if (recovpadsize != 0) {
        // Need to read the latest entry's header, to rewrite it with a
        // zero pad size
        EntryHeaderData pd;
        if (m_d->readEntryHeader(m_d->m_nheadoffs, pd) != CCScanHook::Continue) {
            return false;
        }
        if (int(pd.padsize) != m_d->m_npadsize) {
            m_d->m_reason << "CirCache::put: logic error: bad padsize ";
            return false;
        }
        if (pd.dicsize == 0) {
            // erased entry. Also recover the header space, no need to rewrite
            // the header, we're going to write on it.
            recovpadsize += CIRCACHE_HEADER_SIZE;
        } else {
            LOGDEB("CirCache::put: recov. prev. padsize " << pd.padsize << "\n");
            pd.padsize = 0;
            if (!m_d->writeEntryHeader(m_d->m_nheadoffs, pd)) {
                return false;
            }
            // If we fail between here and the end, the file is broken.
        }
        nwriteoffs = m_d->m_oheadoffs - recovpadsize;
    }

    if (nsize <= recovpadsize) {
        // If the new entry fits entirely in the pad area from the
        // latest one, no need to recycle stuff
        LOGDEB("CirCache::put: new fits in old padsize " << recovpadsize <<"\n");
        npadsize = recovpadsize - nsize;
    } else if (st.st_size < m_d->m_maxsize) {
        // Still growing the file.
        npadsize = 0;
        extending = true;
    } else {
        // Scan the file until we have enough space for the new entry,
        // and determine the pad size up to the 1st preserved entry
        int64_t scansize = nsize - recovpadsize;
        LOGDEB("CirCache::put: scanning for size " << scansize <<
               " from offs " << m_d->m_oheadoffs << "\n");
        CCScanHookSpacer spacer(scansize);
        switch (m_d->scan(m_d->m_oheadoffs, &spacer)) {
        case CCScanHook::Stop:
            LOGDEB("CirCache::put: Scan ok, sizeseen " << spacer.sizeseen<<"\n");
            npadsize = spacer.sizeseen - scansize;
            break;
        case CCScanHook::Eof:
            npadsize = 0;
            extending = true;
            break;
        case CCScanHook::Continue:
        case CCScanHook::Error:
            return false;
        }
        // Take the recycled entries off the multimap
        m_d->khClear(spacer.squashed_udis);
    }

    LOGDEB("CirCache::put: writing " << nsize << " at " << nwriteoffs <<
           " padsize " << npadsize << "\n");

    if (lseek(m_d->m_fd, nwriteoffs, 0) != nwriteoffs) {
        m_d->m_reason << "CirCache::put: lseek failed: " << errno;
        return false;
    }

    char head[CIRCACHE_HEADER_SIZE];
    memset(head, 0, CIRCACHE_HEADER_SIZE);
    snprintf(head, CIRCACHE_HEADER_SIZE,
             headerformat, dic.size(), datalen, npadsize, flags);
    struct iovec vecs[3];
    vecs[0].iov_base = head;
    vecs[0].iov_len = CIRCACHE_HEADER_SIZE;
    vecs[1].iov_base = (void *)dic.c_str();
    vecs[1].iov_len = dic.size();
    vecs[2].iov_base = (void *)datap;
    vecs[2].iov_len = datalen;
    if (writev(m_d->m_fd, vecs, 3) !=  nsize) {
        m_d->m_reason << "put: write failed. errno " << errno;
        if (extending)
            if (ftruncate(m_d->m_fd, m_d->m_oheadoffs) == -1) {
                m_d->m_reason << "put: ftruncate failed. errno " << errno;
            }
        return false;
    }

    m_d->khEnter(udi, nwriteoffs);

    // Update first block information
    m_d->m_nheadoffs = nwriteoffs;
    m_d->m_npadsize  = npadsize;
    // New oldest header is the one just after the one we just wrote.
    m_d->m_oheadoffs = nwriteoffs + nsize + npadsize;
    if (nwriteoffs + nsize >= m_d->m_maxsize) {
        // Max size or top of file reached, next write at BOT.
        m_d->m_oheadoffs = CIRCACHE_FIRSTBLOCK_SIZE;
    }
    return m_d->writefirstblock();
}

bool CirCache::rewind(bool& eof)
{
    if (nullptr == m_d) {
        LOGERR("CirCache::rewind: null data\n");
        return false;
    }

    eof = false;

    int64_t fsize = lseek(m_d->m_fd, 0, SEEK_END);
    if (fsize == (int64_t) - 1) {
        LOGERR("CirCache::rewind: seek to EOF failed\n");
        return false;
    }
    // Read oldest header. This is either at the position pointed to
    // by oheadoffs, or after the first block if the file is still
    // growing.
    if (m_d->m_oheadoffs == fsize) {
        m_d->m_itoffs = CIRCACHE_FIRSTBLOCK_SIZE;
    } else {
        m_d->m_itoffs = m_d->m_oheadoffs;
    }
    CCScanHook::status st = m_d->readEntryHeader(m_d->m_itoffs, m_d->m_ithd);

    switch (st) {
    case CCScanHook::Eof:
        eof = true;
        return false;
    case CCScanHook::Continue:
        return true;
    default:
        return false;
    }
}

bool CirCache::next(bool& eof)
{
    if (nullptr == m_d) {
        LOGERR("CirCache::next: null data\n");
        return false;
    }

    eof = false;

    // Skip to next header, using values stored from previous one
    m_d->m_itoffs += CIRCACHE_HEADER_SIZE + m_d->m_ithd.dicsize +
        m_d->m_ithd.datasize + m_d->m_ithd.padsize;

    // Looped back ?
    if (m_d->m_itoffs == m_d->m_oheadoffs) {
        eof = true;
        return false;
    }

    // Read. If we hit physical eof, fold.
    CCScanHook::status st = m_d->readEntryHeader(m_d->m_itoffs, m_d->m_ithd);
    if (st == CCScanHook::Eof) {
        m_d->m_itoffs = CIRCACHE_FIRSTBLOCK_SIZE;
        if (m_d->m_itoffs == m_d->m_oheadoffs) {
            // Then the file is not folded yet (still growing)
            eof = true;
            return false;
        }
        st = m_d->readEntryHeader(m_d->m_itoffs, m_d->m_ithd);
    }

    if (st == CCScanHook::Continue) {
        return true;
    }
    return false;
}

bool CirCache::getCurrentUdi(string& udi)
{
    if (nullptr == m_d) {
        LOGERR("CirCache::getCurrentUdi: null data\n");
        return false;
    }

    if (!m_d->readHUdi(m_d->m_itoffs, m_d->m_ithd, udi)) {
        return false;
    }
    return true;
}

bool CirCache::getCurrent(string& udi, string& dic, string *data)
{
    if (nullptr == m_d) {
        LOGERR("CirCache::getCurrent: null data\n");
        return false;
    }
    if (!m_d->readDicData(m_d->m_itoffs, m_d->m_ithd, dic, data)) {
        return false;
    }

    ConfSimple conf(dic, 1);
    conf.get("udi", udi, cstr_null);
    return true;
}

// Send all entries from occ to callback. occ is already open.
static bool copyall(std::shared_ptr<CirCache> occ,
                    std::function<bool(const std::string, ConfSimple*, const std::string&)> cb,
                    int& nentries, ostringstream& msg)
{
    bool eof = false;
    if (!occ->rewind(eof)) {
        if (!eof) {
            msg << "Initial rewind failed" << endl;
            return false;
        }
    }
    nentries = 0;
    while (!eof) {
        string udi, sdic, data;
        if (!occ->getCurrent(udi, sdic, &data)) {
            msg << "getCurrent failed: " << occ->getReason() << endl;
            return false;
        }
        // Shouldn't getcurrent deal with this ?
        if (sdic.size() == 0) {
            //cerr << "Skip empty entry" << endl;
            occ->next(eof);
            continue;
        }
        ConfSimple dic(sdic);
        if (!dic.ok()) {
            msg << "Could not parse entry attributes dic" << endl;
            return false;
        }
        //cerr << "UDI: " << udi << endl;
        if (!cb(udi, &dic, data)) {
            string err;
            catstrerror(&err, "", errno);
            msg << "put failed: errno " << err << " for [" << sdic << "]" << endl;
            return false;
        }
        nentries++;
        occ->next(eof);
    }
    return true;
}

int CirCache::appendCC(const string& ddir, const string& sdir, string *reason)
{
    ostringstream msg;
    // Open source file
    std::shared_ptr<CirCache> occ(new CirCache(sdir));
    if (!occ->open(CirCache::CC_OPREAD)) {
        if (reason) {
            msg << "Open failed in " << sdir << " : " << occ->getReason() << endl;
            *reason = msg.str();
        }
        return -1;
    }

    // Possibly resize dest. If the dest is currently recycling, it
    // will keep on. This only avoids erasing entries in dest if it is
    // currently writing at EOF (growing), which will be the case if
    // we are using this to compact existing file (the dest was just
    // created for the purpose).
    int64_t dstavail{0}, dstmaxsize{0};
    bool isunique;
    // Check dest size
    {
        std::shared_ptr<CirCache> ncc(new CirCache(ddir));
        if (!ncc->open(CirCache::CC_OPREAD)) {
            if (reason) {
                msg << "Open failed in " << ddir << " : " << ncc->getReason() << endl;
                *reason = msg.str();
            }
            return -1;
        }
        dstmaxsize = ncc->m_d->m_maxsize;
        dstavail =  dstmaxsize - ncc->m_d->m_nheadoffs;
        isunique = ncc->m_d->m_uniquentries;
    }
    if (dstavail < occ->size()) {
        std::shared_ptr<CirCache> ncc(new CirCache(ddir));
        auto nsize = dstmaxsize + (occ->size() - dstavail) + 5*1000*1000;
        LOGDEB1("CirCache::appendCC: Dstmaxsize " << displayableBytes(dstmaxsize) << " dstavail "<<
                displayableBytes(dstavail) << " org size " << displayableBytes(occ->size()) <<
                " nsize " << displayableBytes(nsize) << "\n");
        if (!ncc->create(nsize, isunique ? CC_CRUNIQUE : CC_CRNONE)) {
            if (reason) {
                msg << "Open failed in " << ddir << " : " << ncc->getReason() << endl;
                *reason = msg.str();
            }
            return -1;
        }
    }
    
    // Open dest file
    std::shared_ptr<CirCache> ncc(new CirCache(ddir));
    if (!ncc->open(CirCache::CC_OPWRITE)) {
        if (reason) {
            msg << "Open failed in " << ddir << " : " << ncc->getReason() << endl;
            *reason = msg.str();
        }
        return -1;
    }

    int nentries;
    std::function<bool(const std::string, ConfSimple*, const std::string&)> cb =
        std::bind(&CirCache::put, ncc, _1, _2, _3, 0);
    if (!copyall(occ, cb, nentries, msg)) {
        msg << " " << ncc->getReason() << "\n";
        LOGERR(msg.str());
        if (reason) {
            *reason = msg.str();
        }
        return -1;
    }

    return nentries;
}

bool CirCache::compact(const std::string& dir, std::string *reason)
{
    ostringstream msg;
    msg << "CirCache::compact: ";
    // Open source file
    std::shared_ptr<CirCache> occ(new CirCache(dir));
    if (!occ->open(CirCache::CC_OPREAD)) {
        msg << "open failed in " << dir << " : " << occ->getReason() << "\n";
        LOGERR(msg.str());
        if (reason) {
            *reason = msg.str();
        }
        return false;
    }
    long long avmbs;
    if (fsocc(dir, nullptr, &avmbs) && avmbs * 1024 * 1024 < 1.2 * occ->size()) {
        msg << "not enough space on file system";
        LOGERR(msg.str() <<"\n");
        if (reason) {
            *reason = msg.str();
        }
        return false;
    }
    std::string ndir = path_cat(dir, "tmpcopy");
    if (!path_makepath(dir, 0700)) {
        msg << "path_makepath failed with errno " << errno;
        LOGERR(msg.str() << "\n");
        if (reason) {
            *reason = msg.str();
        }
        return false;
    }
        
    std::shared_ptr<CirCache> ncc(new CirCache(ndir));
    if (!ncc->create(occ->size(), occ->uniquentries() ? CC_CRUNIQUE : CC_CRNONE)) {
        msg << "open failed in " << ndir << " : " << ncc->getReason();
        LOGERR(msg.str() << "\n");
        if (reason) {
            *reason = msg.str();
        }
        return false;
    }
    int nentries;
    std::function<bool(const std::string, ConfSimple*, const std::string&)> cb =
        std::bind(&CirCache::put, ncc, _1, _2, _3, 0);
    if (!copyall(occ, cb, nentries, msg)) {
        msg << " " << ncc->getReason();
        LOGERR(msg.str() << "\n");
        if (reason) {
            *reason = msg.str();
        }
        return false;
    }

    // Close both
    occ.reset();
    ncc.reset();
    // Rename new to old
    std::string r;
    std::string nfile = path_cat(ndir, "circache.crch").c_str();
    std::string ofile = path_cat(dir, "circache.crch").c_str();
    if (!renameormove(nfile.c_str(), ofile.c_str(), r)) {
        msg << "rename: " << r;
        LOGERR(msg.str() << "\n");
        if (reason) {
            *reason = msg.str();
        }
        return false;
    }

    // Cleanup
    wipedir(ndir, true);
    return true;
}

class CCDataToFile {
public:
    CCDataToFile(const std::string dd)
        : m_dir(dd) {}
    bool putFile(const std::string& udi, const ConfSimple *dicp, const std::string& data);
    std::string& getReason() {return m_reason;}
private:
    std::string m_dir;
    std::string m_reason;
};

bool CCDataToFile::putFile(const std::string& udi, const ConfSimple *dicp, const std::string& data)
{
#if 0
    std::ostringstream deb;
    dicp->write(deb);
    LOGDEB("CCDataToFile::putFile: udi " << udi << " dic " << deb.str() <<
           " datasize " << data.size() << "\n");
#endif
    std::string hash = MD5Hex(udi);
    std::string dsuff;
    std::string mt;
    dicp->get("mimetype", mt);
    if (mt == "text/html") {
        dsuff = ".html";
    } else if (mt == "application/pdf") {
        dsuff = ".pdf";
    } else {
        dsuff = ".xxx";
    }

    int dupnum = 1;
    std::string fn;
    for (;;) {
        fn = path_cat(m_dir, "circache-" + hash + "-" + lltodecstr(dupnum) + dsuff);
        if (path_exists(fn)) {
            dupnum++;
        } else {
            break;
        }
    }
    if (!stringtofile(data, fn.c_str(), m_reason)) {
        return false;
    }
    // Try to reset the original mtime on the data file.
    std::string sfmtime;
    if (dicp->get("fmtime", sfmtime)) {
        time_t fmtime = atoll(sfmtime.c_str());
        if (fmtime) {
            struct path_timeval tv[2];
            tv[0] = tv[1] = {fmtime, 0};
            path_utimes(fn, tv);
        }
    }

    fn = path_cat(m_dir, "circache-" + hash + "-" + lltodecstr(dupnum) + ".dic");
    std::ostringstream str;
    dicp->write(str);
    if (!stringtofile(str.str(), fn.c_str(), m_reason)) {
        return false;
    }
    return true;
}

bool CirCache::burst(const std::string& ccdir, const std::string destdir, std::string *reason)
{
    ostringstream msg;
    msg << "CirCache::burst: ";
    // Open source file
    std::shared_ptr<CirCache> occ(new CirCache(ccdir));
    if (!occ->open(CirCache::CC_OPREAD)) {
        msg << "open failed in " << ccdir << " : " << occ->getReason() << "\n";
        LOGERR(msg.str());
        if (reason) {
            *reason = msg.str();
        }
        return false;
    }

    // This test may now actually be very optimistic because of compression ?
    long long avmbs;
    if (fsocc(destdir, nullptr, &avmbs) && avmbs * 1024 * 1024 < 1.2 * occ->size()) {
        msg << "not enough space on file system";
        LOGERR(msg.str() <<"\n");
        if (reason) {
            *reason = msg.str();
        }
        return false;
    }

    if (!path_makepath(destdir, 0700)) {
        msg << "path_makepath failed with errno " << errno;
        LOGERR(msg.str() << "\n");
        if (reason) {
            *reason = msg.str();
        }
        return false;
    }

    int nentries;
    CCDataToFile copier(destdir);
    std::function<bool(const std::string, ConfSimple*, const std::string&)> cb =
        std::bind(&CCDataToFile::putFile, copier, _1, _2, _3);
    if (!copyall(occ, cb, nentries, msg)) {
        msg << " " << copier.getReason();
        LOGERR(msg.str() << "\n");
        if (reason) {
            *reason = msg.str();
        }
        return false;
    }

    return true;
}
