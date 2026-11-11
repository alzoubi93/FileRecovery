// محرك الاستخلاص (File Carving): بدون أي اعتماد على JNI حتى يمكن اختباره على الحاسوب.
#pragma once
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <vector>

#ifndef CARVER_MAX_FILE
#define CARVER_MAX_FILE (32u * 1024 * 1024)  // أقصى حجم لملف مستعاد
#endif

namespace carver {

constexpr size_t kMaxFileSize = CARVER_MAX_FILE;
constexpr size_t kBufSize = kMaxFileSize * 2;
constexpr size_t kCluster = 4096;               // حجم الكتلة المفترض لنظام الملفات (ext4/f2fs)
constexpr size_t kGapMaxChunk = 64 * 1024;      // أكبر كتلة PNG نحاول إعادة تجميعها
constexpr size_t kGapWindow = 4u << 20;         // مدى البحث عن بقية الملف بعد الفجوة
constexpr long kNeedMore = -1;  // البيانات لم تصل بعد، اقرأ المزيد ثم أعد المحاولة
constexpr long kLarge = -2;     // ملف فيديو أكبر من الذاكرة المؤقتة: يُكتب تدفقياً
#ifndef CARVER_HUGE_CAP
#define CARVER_HUGE_CAP (4ull * 1024 * 1024 * 1024)  // أقصى حجم لصندوق في وضع التدفق
#endif
constexpr uint64_t kHugeCap = CARVER_HUGE_CAP;

inline std::atomic<long long> gBytes{0};
inline std::atomic<int> gCount{0};
inline std::atomic<bool> gCancel{false};

struct Ctx {
    const uint8_t* p;
    size_t lim;  // ما يمكن الاعتماد عليه من البيانات (≤ kMaxFileSize)
    bool canWait;  // هل قد تصل بيانات إضافية؟
    bool alignOk;  // هل بداية الملف على حدّ كتلة؟ شرط لافتراض أن الأجزاء المجزّأة تبدأ عند حدود الكتل
    long starve() const { return canWait ? kNeedMore : 0; }
};

inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline uint32_t le32(const uint8_t* p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
inline uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

// بايتات البداية المحتملة لأي صيغة: جدول بحث بدل switch لتفادي أخطاء التنبؤ بالقفز على البيانات العشوائية
inline const std::array<bool, 256> kFirstByte = [] {
    std::array<bool, 256> t{};
    for (int c : std::initializer_list<int>{0xFF, 0x89, '%', 'R', 'G', 'P', 0x00}) t[c] = true;
    return t;
}();

// ---------- JPEG: تحليل المقاطع، فلا تُخدعنا الصور المصغّرة داخل EXIF ----------
inline long parseJpeg(const Ctx& c) {
    const uint8_t* p = c.p;
    size_t o = 2;
    while (true) {
        if (o + 2 > c.lim) return c.starve();
        if (p[o] != 0xFF) return 0;
        uint8_t m = p[o + 1];
        if (m == 0xFF) { ++o; continue; }
        if (m == 0xD9) return (long)(o + 2);
        if (m == 0x01 || m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) { o += 2; continue; }
        if (o + 4 > c.lim) return c.starve();
        size_t L = (size_t)p[o + 2] << 8 | p[o + 3];
        if (L < 2) return 0;
        if (o + 2 + L > c.lim) return c.starve();
        o += 2 + L;
        if (m == 0xDA) {  // بيانات الصورة المضغوطة حتى المقطع التالي
            while (true) {
                const void* f = o < c.lim ? memchr(p + o, 0xFF, c.lim - o) : nullptr;
                if (!f) return c.starve();
                o = (const uint8_t*)f - p;
                if (o + 2 > c.lim) return c.starve();
                uint8_t n = p[o + 1];
                if (n == 0x00 || (n >= 0xD0 && n <= 0xD7)) { o += 2; continue; }
                if (n == 0xFF) { ++o; continue; }
                break;
            }
        }
    }
}

inline uint32_t crcUpdate(uint32_t c, const uint8_t* d, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t x = i;
            for (int k = 0; k < 8; ++k) x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1;
            t[i] = x;
        }
        return t;
    }();
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}
inline uint32_t crc32(const uint8_t* d, size_t n) { return ~crcUpdate(0xFFFFFFFFu, d, n); }

// ---------- PNG: المرور على الـ chunks مع التحقق من CRC، وإعادة تجميع الملف المجزّأ ----------
// كل كتلة PNG لها CRC، فنستطيع إثبات صحة التجميع يقيناً (احتمال الخطأ 2^-32) بدل التخمين.
// عند فشل CRC نجرّب: أن الكتلة انقطعت عند حدّ كتلة قرص ثم تكملها قطعة لاحقة ضمن نافذة البحث.
struct Seg { size_t off, len; };
inline thread_local std::vector<Seg> gSegs;  // قطع الملف المجزّأ بالترتيب (فارغة إن كان متصلاً)

inline bool pngTypeOk(const uint8_t* t) {
    for (int k = 0; k < 4; ++k)
        if (!((t[k] >= 'A' && t[k] <= 'Z') || (t[k] >= 'a' && t[k] <= 'z'))) return false;
    return true;
}

inline long parsePng(const Ctx& c) {
    const uint8_t* p = c.p;
    gSegs.clear();
    size_t q = 8, segStart = 0, asmBefore = 0;
    bool first = true, fragmented = false;
    int gaps = 0;
    auto asmAt = [&](size_t x) { return asmBefore + (x - segStart); };
    auto closeSeg = [&](size_t end, size_t next) {
        gSegs.push_back({segStart, end - segStart});
        asmBefore += end - segStart;
        segStart = next;
        fragmented = true;
    };
    auto finish = [&](size_t end) -> long {
        if (!fragmented) return (long)end;
        gSegs.push_back({segStart, end - segStart});
        return (long)(asmBefore + (end - segStart));
    };

    while (true) {
        if (q + 12 > c.lim) return c.starve();
        uint32_t len = be32(p + q);
        const uint8_t* t = p + q + 4;
        bool hdrOk = len <= kMaxFileSize && pngTypeOk(t) && (!first || (!memcmp(t, "IHDR", 4) && len == 13));
        size_t total = 12 + (size_t)len;
        if (hdrOk) {
            if (q + total > c.lim) return c.starve();
            if (crc32(t, 4 + len) == be32(p + q + 8 + len)) {
                first = false;
                if (!memcmp(t, "IEND", 4)) return finish(q + total);
                q += total;
                continue;
            }
        }
        // الكتلة تالفة أو رأسها غير صالح: قد يكون الملف مجزّأً
        if (!c.alignOk || ++gaps > 512) return 0;
        size_t Aq = asmAt(q);
        bool found = false;
        if (hdrOk) {  // الانقطاع داخل الكتلة
            if (len > kGapMaxChunk) return 0;
            size_t b0 = (kCluster - Aq % kCluster) % kCluster;
            if (b0 < 8) b0 += kCluster;  // الرأس (8 بايت) يجب أن يكون في القطعة الأولى
            for (size_t b = b0; b + 4 <= total && !found; b += kCluster) {
                uint32_t st = crcUpdate(0xFFFFFFFFu, t, b - 4);  // بايتات [4, b) من الكتلة
                size_t rest = total - b;                          // المتبقي من الكتلة بما فيه CRC
                size_t c0 = (q + b) / kCluster * kCluster + kCluster;
                for (; c0 + rest <= c.lim && c0 <= q + b + kGapWindow; c0 += kCluster) {
                    if (~crcUpdate(st, p + c0, rest - 4) == be32(p + c0 + rest - 4)) {
                        closeSeg(q + b, c0);
                        q = c0 + rest;
                        found = true;
                        break;
                    }
                }
            }
        } else {  // الانقطاع عند حدّ كتلة PNG تماماً: نبحث عن بداية كتلة صالحة لاحقة
            if (Aq % kCluster != 0 || q % kCluster != 0) return 0;
            for (size_t c0 = q + kCluster; c0 + 12 <= c.lim && c0 <= q + kGapWindow; c0 += kCluster) {
                uint32_t l2 = be32(p + c0);
                const uint8_t* t2 = p + c0 + 4;
                if (l2 > kGapMaxChunk || !pngTypeOk(t2) || c0 + 12 + l2 > c.lim) continue;
                if (crc32(t2, 4 + l2) == be32(p + c0 + 8 + l2)) {
                    closeSeg(q, c0);
                    q = c0;
                    found = true;
                    break;
                }
            }
        }
        if (!found) return c.starve();
        first = false;
    }
}

// ---------- WEBP: الحجم مكتوب في الترويسة ----------
inline long parseWebp(const Ctx& c) {
    if (c.lim < 12) return c.starve();
    if (memcmp(c.p + 8, "WEBP", 4) != 0) return 0;
    uint64_t total = (uint64_t)le32(c.p + 4) + 8;
    if (total < 20 || total > kMaxFileSize) return 0;
    if (total > c.lim) return c.starve();
    return (long)total;
}

// ---------- PDF: من الترويسة إلى أول %%EOF ----------
inline long parsePdf(const Ctx& c) {
    if (c.lim < 9) return c.starve();
    if (c.p[5] < '1' || c.p[5] > '2' || c.p[6] != '.') return 0;
    const void* e = memmem(c.p + 8, c.lim - 8, "%%EOF", 5);
    if (!e) return c.starve();
    size_t end = (const uint8_t*)e - c.p + 5;
    for (int k = 0; k < 2 && end < c.lim && (c.p[end] == '\r' || c.p[end] == '\n'); ++k) ++end;
    return (long)end;
}

// ---------- كشف ترويسات ملفات أخرى داخل بيانات mdat (علامة أن الفيديو مبتور/مستبدل) ----------
inline long findForeignHeader(const uint8_t* d, size_t n) {
    long best = -1;
    auto upd = [&](long k) { if (k >= 0 && (best < 0 || k < best)) best = k; };
    auto at = [&](const void* f) { return f ? (long)((const uint8_t*)f - d) : -1L; };
    upd(at(memmem(d, n, "\x89PNG\r\n\x1a\n", 8)));
    upd(at(memmem(d, n, "%PDF-1.", 7)));
    for (const auto& sig : {std::make_pair("JFIF\0", (size_t)5), std::make_pair("Exif\0\0", (size_t)6)}) {
        size_t from = 0;
        while (from + sig.second <= n) {
            const void* f = memmem(d + from, n - from, sig.first, sig.second);
            if (!f) break;
            long pos = at(f), k = pos - 6;
            if (k >= 0 && d[k] == 0xFF && d[k + 1] == 0xD8 && d[k + 2] == 0xFF) { upd(k); break; }
            from = pos + 1;
        }
    }
    upd(at(memmem(d, n, "GIF89a", 6)));
    upd(at(memmem(d, n, "GIF87a", 6)));
    for (size_t from = 0; from + 6 <= n;) {  // ZIP/DOCX/APK: PK\3\4 متبوعة بإصدار شائع
        const void* f = memmem(d + from, n - from, "PK\x03\x04", 4);
        if (!f) break;
        long pos = at(f);
        if ((size_t)pos + 6 <= n && d[pos + 5] == 0 &&
            (d[pos + 4] == 0x0A || d[pos + 4] == 0x0E || d[pos + 4] == 0x14 || d[pos + 4] == 0x1E ||
             d[pos + 4] == 0x2D)) { upd(pos); break; }
        from = pos + 1;
    }
    for (size_t from = 0; from + 4 <= n;) {  // WEBP: RIFF + حجم + WEBP
        const void* f = memmem(d + from, n - from, "WEBP", 4);
        if (!f) break;
        long pos = at(f);
        if (pos >= 8 && memcmp(d + pos - 8, "RIFF", 4) == 0) { upd(pos - 8); break; }
        from = pos + 1;
    }
    size_t from = 0;
    while (from + 4 <= n) {
        const void* f = memmem(d + from, n - from, "ftyp", 4);
        if (!f) break;
        long pos = at(f), k = pos - 4;
        if (k >= 0 && d[k] == 0 && d[k + 1] == 0 && be32(d + k) >= 16 && be32(d + k) <= 64) { upd(k); break; }
        from = pos + 1;
    }
    return best;
}

// ---------- ISO-BMFF (MP4 / MOV / HEIC / 3GP): المرور على الصناديق (boxes) ----------
inline bool knownBox(const uint8_t* t) {
    static const char* k[] = {"ftyp", "moov", "mdat", "free", "skip", "wide", "meta", "uuid",
                              "moof", "mfra", "styp", "sidx", "junk", "pnot"};
    for (auto s : k) if (memcmp(t, s, 4) == 0) return true;
    return false;
}

inline long parseIsoBmff(const Ctx& c, const char** extOut) {
    const uint8_t* p = c.p;
    if (c.lim < 16) return c.starve();
    uint32_t first = be32(p);
    if (first < 16 || first > 64) return 0;
    const uint8_t* br = p + 8;  // العلامة الرئيسية
    if (!memcmp(br, "heic", 4) || !memcmp(br, "heix", 4) || !memcmp(br, "mif1", 4) || !memcmp(br, "heif", 4)) *extOut = "heic";
    else if (!memcmp(br, "qt  ", 4)) *extOut = "mov";
    else if (!memcmp(br, "M4A ", 4)) *extOut = "m4a";
    else if (!memcmp(br, "3gp", 3)) *extOut = "3gp";
    else *extOut = "mp4";

    size_t o = 0;
    bool mdat = false;
    while (true) {
        if (o + 8 > c.lim) return mdat && !c.canWait ? (long)o : c.starve();
        const uint8_t* t = p + o + 4;
        if (!knownBox(t)) break;
        uint64_t sz = be32(p + o);
        size_t hdr = 8;
        if (sz == 1) {
            if (o + 16 > c.lim) return c.starve();
            sz = be64(p + o + 8);
            hdr = 16;
        } else if (sz == 0) {
            return 0;  // صندوق يمتد حتى نهاية الملف: حدوده غير معروفة
        }
        if (sz < hdr || sz > kHugeCap) return 0;
        if (o + sz > kMaxFileSize) return kLarge;  // لا يتسع في الذاكرة المؤقتة
        if (o + sz > c.lim) return c.starve();
        if (!memcmp(t, "mdat", 4)) {
            mdat = true;
            // وجود ترويسة ملف آخر داخل mdat يعني أن الفيديو مبتور/مستبدل: نرفضه ولا نبتلع ما بعده
            if (sz > hdr && findForeignHeader(p + o + hdr, sz - hdr) >= 0) return 0;
        }
        o += sz;
    }
    return mdat ? (long)o : 0;
}

// ---------- GIF: المرور على الكتل حتى المحرف 0x3B ----------
inline int gifSubBlocks(const uint8_t* p, size_t lim, size_t& o) {  // 1 تم، -1 بيانات ناقصة
    while (true) {
        if (o >= lim) return -1;
        uint8_t n = p[o++];
        if (n == 0) return 1;
        if (o + n > lim) return -1;
        o += n;
    }
}

inline long parseGif(const Ctx& c) {
    const uint8_t* p = c.p;
    if (c.lim < 13) return c.starve();
    if (!(p[4] == '7' || p[4] == '9') || p[5] != 'a') return 0;
    size_t o = 13;
    if (p[10] & 0x80) o += 3u * (1u << ((p[10] & 7) + 1));
    bool frame = false;
    while (true) {
        if (o >= c.lim) return c.starve();
        uint8_t b = p[o];
        if (b == 0x3B) return frame ? (long)(o + 1) : 0;
        if (b == 0x21) {
            if (o + 2 > c.lim) return c.starve();
            o += 2;
            if (gifSubBlocks(p, c.lim, o) < 0) return c.starve();
        } else if (b == 0x2C) {
            if (o + 10 > c.lim) return c.starve();
            uint8_t f = p[o + 9];
            o += 10;
            if (f & 0x80) o += 3u * (1u << ((f & 7) + 1));
            if (o + 1 > c.lim) return c.starve();
            o += 1;  // LZW minimum code size
            if (gifSubBlocks(p, c.lim, o) < 0) return c.starve();
            frame = true;
        } else {
            return 0;
        }
    }
}

// ---------- ZIP (وكذلك docx / xlsx / pptx / apk): البحث عن EOCD الصحيح ----------
// نتحقق أن (إزاحة الدليل المركزي + حجمه) تساوي موضع EOCD، حتى لا يخدعنا zip مضمّن داخل zip.
inline uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

inline long parseZip(const Ctx& c, const char** extOut) {
    const uint8_t* p = c.p;
    if (c.lim < 30) return c.starve();
    size_t from = 30;
    while (true) {
        if (from + 4 > c.lim) return c.starve();
        const uint8_t* e = (const uint8_t*)memmem(p + from, c.lim - from, "PK\x05\x06", 4);
        if (!e) return c.starve();
        size_t pos = e - p;
        if (pos + 22 > c.lim) return c.starve();
        uint32_t cdSize = le32(e + 12), cdOff = le32(e + 16);
        if ((uint64_t)cdSize + cdOff == pos) {
            size_t total = pos + 22 + le16(e + 20);
            if (total > c.lim) return c.starve();
            auto has = [&](const char* s) { return memmem(p, total, s, strlen(s)) != nullptr; };
            if (has("word/document.xml")) *extOut = "docx";
            else if (has("xl/workbook.xml")) *extOut = "xlsx";
            else if (has("ppt/presentation.xml")) *extOut = "pptx";
            else if (has("AndroidManifest.xml") && has("classes.dex")) *extOut = "apk";
            else *extOut = "zip";
            return (long)total;
        }
        from = pos + 1;
    }
}

// ---------- الحفظ والحلقة الرئيسية ----------
inline bool saveFile(const std::string& dir, int idx, const char* ext, const uint8_t* d, size_t len) {
    char name[512];
    snprintf(name, sizeof(name), "%s/recovered_%05d.%s", dir.c_str(), idx, ext);
    FILE* f = fopen(name, "wb");
    if (!f) return false;
    size_t w = fwrite(d, 1, len, f);
    fclose(f);
    return w == len;
}

// يعيد: >0 طول الملف، 0 غير صالح، kNeedMore للمزيد من البيانات
inline long detect(const uint8_t* p, size_t avail, bool eof, size_t absOffset, const char*& ext, size_t& minLen) {
    gSegs.clear();
    Ctx c{p, avail < kMaxFileSize ? avail : kMaxFileSize, !eof && avail < kMaxFileSize, absOffset % kCluster == 0};
    if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF && p[3] >= 0xC0) {
        ext = "jpg"; minLen = 300; return parseJpeg(c);
    }
    if (p[0] == 0x89 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) {
        ext = "png"; minLen = 60; return parsePng(c);
    }
    if (p[0] == '%' && !memcmp(p, "%PDF-", 5)) {
        ext = "pdf"; minLen = 100; return parsePdf(c);
    }
    if (p[0] == 'R' && !memcmp(p, "RIFF", 4)) {
        ext = "webp"; minLen = 20; return parseWebp(c);
    }
    if (p[0] == 'G' && !memcmp(p, "GIF8", 4)) {
        ext = "gif"; minLen = 30; return parseGif(c);
    }
    if (p[0] == 'P' && p[1] == 'K' && p[2] == 3 && p[3] == 4) {
        minLen = 100; return parseZip(c, &ext);
    }
    if (p[0] == 0x00 && p[1] == 0x00 && p[4] == 'f' && !memcmp(p + 4, "ftyp", 4)) {
        minLen = 1000; return parseIsoBmff(c, &ext);
    }
    return 0;
}

// ---------- وضع التدفق للفيديوهات الكبيرة: نمشي على الصناديق ونكتب مباشرة من المصدر إلى الملف ----------
// داخل mdat نراقب ظهور ترويسة ملف آخر (PNG/PDF/JPEG/ftyp): إن ظهرت فالفيديو مبتور، فنتخلى عنه
// ونعيد البيانات المقروءة إلى الذاكرة المؤقتة ليُكمل المسح منها بدل أن نبتلعها.
// القيمة المعادة: 1 نجاح، 0 فشل، 2 فشل مع إعادة التموضع (لا تتقدّم يدوياً).
inline int streamIso(int fd, std::vector<uint8_t>& buf, size_t& len, size_t& i, bool& eof,
                     const std::string& dir, int idx, const char* ext) {
    char name[512];
    snprintf(name, sizeof(name), "%s/recovered_%05d.%s", dir.c_str(), idx, ext);
    FILE* out = fopen(name, "wb");
    if (!out) return 0;
    constexpr size_t kCarry = 15;
    // القطعة يجب أن تتسع (مع الحمل) داخل buf حتى نستطيع إعادتها إليه عند التخلي عن الفيديو
    const size_t kChunk = std::min<size_t>(1u << 20, buf.size() / 2);
    std::vector<uint8_t> work(kCarry + kChunk);
    size_t carry = 0;
    bool mdat = false, moov = false;

    auto refill = [&]() {
        if (eof || len - i >= 16) return;
        memmove(buf.data(), buf.data() + i, len - i);
        len -= i; i = 0;
        while (!eof && len < buf.size()) {
            ssize_t n = read(fd, buf.data() + len, buf.size() - len);
            if (n > 0) { len += n; gBytes += n; }
            else if (n < 0 && errno == EINTR) continue;
            else eof = true;
        }
    };
    auto fail = [&](int code) { fclose(out); remove(name); return code; };

    while (!gCancel) {
        refill();
        if (len - i < 8) break;
        const uint8_t* b = buf.data() + i;
        if (!knownBox(b + 4)) break;
        uint64_t sz = be32(b);
        size_t hdr = 8;
        if (sz == 1) { if (len - i < 16) break; sz = be64(b + 8); hdr = 16; }
        else if (sz == 0) break;
        if (sz < hdr || sz > kHugeCap) break;
        bool isMdat = !memcmp(b + 4, "mdat", 4), isMoov = !memcmp(b + 4, "moov", 4);

        uint64_t remaining = sz;
        size_t take = (uint64_t)(len - i) < remaining ? len - i : (size_t)remaining;
        if (isMdat && take > hdr) {
            long k = findForeignHeader(buf.data() + i + hdr, take - hdr);
            if (k >= 0) { i += hdr + k; return fail(2); }
        }
        if (fwrite(buf.data() + i, 1, take, out) != take) return fail(0);
        i += take; remaining -= take;
        carry = 0;
        while (remaining > 0 && !eof) {
            size_t want = remaining < kChunk ? (size_t)remaining : kChunk;
            ssize_t n = read(fd, work.data() + kCarry, want);
            if (n > 0) {
                gBytes += n;
                if (isMdat) {
                    const uint8_t* region = work.data() + kCarry - carry;
                    size_t rlen = carry + n;
                    long k = findForeignHeader(region, rlen);
                    if (k >= 0) {
                        size_t rest = rlen - k;
                        memcpy(buf.data(), region + k, rest);
                        len = rest; i = 0;
                        return fail(2);
                    }
                    carry = rlen < kCarry ? rlen : kCarry;
                    memmove(work.data() + kCarry - carry, region + rlen - carry, carry);
                }
                if (fwrite(work.data() + kCarry, 1, n, out) != (size_t)n) return fail(0);
                remaining -= n;
            } else if (n < 0 && errno == EINTR) continue;
            else eof = true;
        }
        if (remaining > 0) return fail(0);  // الصندوق مبتور
        mdat |= isMdat; moov |= isMoov;
    }
    fclose(out);
    if (!(mdat && moov)) { remove(name); return 0; }  // فيديو بلا moov لا يعمل
    return 1;
}

// قراءة متدفقة من ملف أو أنبوب (مثلاً dd بصلاحية root)
inline int carveFd(int fd, const std::string& dir) {
    std::vector<uint8_t> buf(kBufSize);
    size_t len = 0, i = 0;
    bool eof = false;
    gBytes = 0; gCount = 0; gCancel = false;

    while (true) {
        while (!eof && len < buf.size()) {
            ssize_t n = read(fd, buf.data() + len, buf.size() - len);
            if (n > 0) { len += n; gBytes += n; }
            else if (n < 0 && errno == EINTR) continue;
            else eof = true;
        }
        while (i + 16 < len || (eof && i + 8 < len)) {
            if (gCancel) return gCount;
            if (i + 16 > len) { ++i; continue; }  // نهاية الملف: لا يتسع لترويسة
            const uint8_t* q = buf.data() + i;
            if (!kFirstByte[q[0]]) { ++i; continue; }
            bool cand;  // فحص رخيص للبايت الثاني قبل الاستدعاء الأثقل
            switch (q[0]) {
                case 0xFF: cand = q[1] == 0xD8; break;
                case 0x89: cand = q[1] == 'P'; break;
                case '%':  cand = q[1] == 'P'; break;
                case 'R':  cand = q[1] == 'I'; break;
                case 'G':  cand = q[1] == 'I'; break;
                case 'P':  cand = q[1] == 'K'; break;
                case 0x00: cand = q[4] == 'f'; break;
                default:   cand = false;
            }
            if (!cand) { ++i; continue; }
            const char* ext = nullptr;
            size_t minLen = 0;
            size_t absOff = (size_t)(gBytes.load() - (long long)len) + i;  // موضع هذه البايت في المصدر
            long r = detect(buf.data() + i, len - i, eof, absOff, ext, minLen);
            if (r == carver::kNeedMore) break;
            if (r == carver::kLarge) {
                size_t i0 = i;
                int sr = streamIso(fd, buf, len, i, eof, dir, gCount, ext);
                if (sr == 1) ++gCount;
                else if (sr == 0 && i == i0) ++i;
                continue;
            }
            if (r > 0 && (size_t)r >= minLen) {
                if (gSegs.empty()) {
                    if (saveFile(dir, gCount, ext, buf.data() + i, (size_t)r)) ++gCount;
                    i += r;
                } else {  // ملف مجزّأ: نجمع قطعه ثم نتقدّم بمقدار القطعة الأولى فقط
                    std::vector<uint8_t> asmb;
                    asmb.reserve((size_t)r);
                    for (const Seg& sg : gSegs)
                        asmb.insert(asmb.end(), buf.data() + i + sg.off, buf.data() + i + sg.off + sg.len);
                    if (saveFile(dir, gCount, ext, asmb.data(), asmb.size())) ++gCount;
                    i += gSegs.front().len;  // الفجوة قد تحوي ملفات سليمة أخرى، فنواصل مسحها
                }
            } else {
                ++i;
            }
        }
        if (eof) break;
        size_t keep = len - i;
        memmove(buf.data(), buf.data() + i, keep);
        len = keep;
        i = 0;
    }
    return gCount;
}

// مسار قسم آمن لأمر dd: يقبل أقسام vold مثل /dev/block/vold/public:179,1 ويرفض كل ما قد يحقن أوامر
inline bool safeDevicePath(const std::string& s) {
    if (s.rfind("/dev/", 0) != 0 || s.find("..") != std::string::npos) return false;
    for (char c : s)
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '_' || c == '-' || c == '.' || c == ':' || c == ','))
            return false;
    return true;
}

}  // namespace carver
