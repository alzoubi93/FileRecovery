// اختبار المحرك على الحاسوب:
//   g++ -std=c++17 -O2 -DCARVER_MAX_FILE=16384 -I../../main/cpp carver_test.cpp -o carver_test && ./carver_test
// الحد الصغير يجبر المحرك على إعادة تعبئة الذاكرة المؤقتة أثناء المسح لاختبار حدود الكتل.
#include "carver_core.h"
#include <dirent.h>
#include <sys/stat.h>
#include <fstream>
#include <iostream>
#include <random>
#include <map>
#include <set>
#include <sstream>

using Bytes = std::vector<uint8_t>;
static std::mt19937 rng(12345);

static void put(Bytes& b, const void* d, size_t n) { auto* p = (const uint8_t*)d; b.insert(b.end(), p, p + n); }
static void be16(Bytes& b, int v) { b.push_back(v >> 8); b.push_back(v & 255); }
static void be32w(Bytes& b, uint32_t v) { be16(b, v >> 16); be16(b, v & 0xFFFF); }
static Bytes rnd(size_t n, bool avoidFF = false) {
    Bytes b(n);
    for (auto& x : b) { x = rng() & 255; if (avoidFF && x == 0xFF) x = 0x7F; }
    return b;
}

static Bytes makeJpeg(bool withThumb) {
    Bytes b = {0xFF, 0xD8};
    Bytes app0 = {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    b.insert(b.end(), {0xFF, 0xE0}); be16(b, 2 + app0.size()); put(b, app0.data(), app0.size());
    if (withThumb) {  // EXIF يحوي jpeg مصغّراً كاملاً، وفيه FFD9 داخلي
        Bytes thumb = {0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x04, 0x01, 0x02, 0xFF, 0xDA, 0x00, 0x04, 0x00, 0x01,
                       0x11, 0x22, 0xFF, 0xD9};
        b.insert(b.end(), {0xFF, 0xE1}); be16(b, 2 + thumb.size()); put(b, thumb.data(), thumb.size());
    }
    Bytes dqt = rnd(65, true);
    b.insert(b.end(), {0xFF, 0xDB}); be16(b, 2 + dqt.size()); put(b, dqt.data(), dqt.size());
    Bytes sof = rnd(15, true);
    b.insert(b.end(), {0xFF, 0xC0}); be16(b, 2 + sof.size()); put(b, sof.data(), sof.size());
    b.insert(b.end(), {0xFF, 0xDA}); be16(b, 10); Bytes sos = rnd(8, true); put(b, sos.data(), 8);
    Bytes ent = rnd(3000, true);
    ent[100] = 0xFF; ent[101] = 0x00;          // byte stuffing
    ent[500] = 0xFF; ent[501] = 0xD0;          // restart marker
    put(b, ent.data(), ent.size());
    b.insert(b.end(), {0xFF, 0xD9});
    return b;
}

static uint32_t crc32(const Bytes& d) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t x : d) { c ^= x; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}

static Bytes makePng() {
    Bytes b = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* type, const Bytes& d) {
        be32w(b, d.size());
        Bytes td; put(td, type, 4); put(td, d.data(), d.size());
        put(b, td.data(), td.size()); be32w(b, crc32(td));
    };
    chunk("IHDR", rnd(13)); chunk("IDAT", rnd(900)); chunk("IEND", {});
    return b;
}

static void pngChunk(Bytes& b, const char* type, const Bytes& d) {
    be32w(b, d.size());
    Bytes td; put(td, type, 4); put(td, d.data(), d.size());
    put(b, td.data(), td.size()); be32w(b, crc32(td));
}
static Bytes makeBigPng(size_t firstIdat, size_t idatSize, int n) {
    Bytes b = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    pngChunk(b, "IHDR", rnd(13)); pngChunk(b, "IDAT", rnd(firstIdat));
    for (int k = 0; k < n; ++k) pngChunk(b, "IDAT", rnd(idatSize));
    pngChunk(b, "IEND", {});
    return b;
}
static void padTo4096(Bytes& image) { Bytes j = rnd((4096 - image.size() % 4096) % 4096); put(image, j.data(), j.size()); }

// يضع الملف على "القرص" مجزّأً: قطع من 1-3 كتل (4096) تفصلها كتل غريبة؛ وفي أول فجوة ملف JPEG سليم
static void putFragmented(Bytes& image, const Bytes& file, const Bytes* gapFile) {
    padTo4096(image);
    size_t pos = 0; int k = 0;
    while (pos < file.size()) {
        size_t piece = std::min<size_t>(file.size() - pos, 4096 * (1 + k % 3));
        put(image, file.data() + pos, piece); pos += piece;
        if (pos < file.size()) {
            Bytes gap = rnd(4096 * (1 + k % 2));
            if (k == 0 && gapFile) std::copy(gapFile->begin(), gapFile->end(), gap.begin());
            put(image, gap.data(), gap.size());
        }
        ++k;
    }
}
// قطعتان من الملف ثم بيانات غريبة فقط: لا استمرارية، ويجب ألا يُستخرج شيء
static void putFragmentedMissing(Bytes& image, const Bytes& file) {
    padTo4096(image);
    put(image, file.data(), 8192);
    Bytes junk = rnd(4096 * 12); put(image, junk.data(), junk.size());
}

static Bytes makeCorruptPng() {  // بايت واحد تالف داخل IDAT: CRC لن يطابق
    Bytes b = makePng(); b[100] ^= 0x55; return b;
}

static Bytes makeTruncVideo() {  // ترويسة سليمة، mdat يدّعي 150KB لكن يتوفر منه 20KB فقط
    Bytes b; be32w(b, 24); put(b, "ftyp", 4); put(b, "isom", 4); be32w(b, 512); put(b, "isom", 4); put(b, "mp41", 4);
    be32w(b, 150000); put(b, "mdat", 4);
    Bytes body = rnd(20000, true); put(b, body.data(), body.size());
    return b;
}

static Bytes makePdf() {
    Bytes b; std::string h = "%PDF-1.7\n"; put(b, h.data(), h.size());
    Bytes body = rnd(1500, true); put(b, body.data(), body.size());
    std::string t = "\nstartxref\n0\n%%EOF\n"; put(b, t.data(), t.size());
    return b;
}

static Bytes makeWebp() {
    Bytes payload = rnd(2000); Bytes b; put(b, "RIFF", 4);
    uint32_t sz = 4 + payload.size();
    for (int i = 0; i < 4; ++i) b.push_back((sz >> (8 * i)) & 255);
    put(b, "WEBP", 4); put(b, payload.data(), payload.size());
    return b;
}

static void box(Bytes& b, const char* type, size_t total) {
    be32w(b, total); put(b, type, 4);
    Bytes body = rnd(total - 8); put(b, body.data(), body.size());
}
static Bytes makeMp4() {
    Bytes b; be32w(b, 24); put(b, "ftyp", 4); put(b, "isom", 4); be32w(b, 512); put(b, "isom", 4); put(b, "mp41", 4);
    box(b, "moov", 400); box(b, "mdat", 6000);
    return b;
}

static Bytes makeMp4Big(bool moovFirst, size_t mdatSize) {
    Bytes b; be32w(b, 24); put(b, "ftyp", 4); put(b, "isom", 4); be32w(b, 512); put(b, "isom", 4); put(b, "mp41", 4);
    if (moovFirst) box(b, "moov", 400);
    box(b, "mdat", mdatSize);
    if (!moovFirst) box(b, "moov", 400);
    return b;
}
static Bytes makeMp4Multi() {  // كل صندوق أصغر من الحد، لكن المجموع أكبر منه
    Bytes b; be32w(b, 24); put(b, "ftyp", 4); put(b, "isom", 4); be32w(b, 512); put(b, "isom", 4); put(b, "mp41", 4);
    box(b, "moov", 400); box(b, "mdat", 12000); box(b, "free", 12000);
    return b;
}

static void le16w(Bytes& b, int v) { b.push_back(v & 255); b.push_back(v >> 8); }
static void le32w(Bytes& b, uint32_t v) { le16w(b, v & 0xFFFF); le16w(b, v >> 16); }

static Bytes makeZip(const std::vector<std::pair<std::string, Bytes>>& entries, const std::string& comment) {
    Bytes b, cd;
    for (auto& [name, data] : entries) {
        uint32_t off = b.size();
        put(b, "PK\x03\x04", 4); le16w(b, 20); le16w(b, 0); le16w(b, 0); le16w(b, 0); le16w(b, 0);
        le32w(b, 0x12345678); le32w(b, data.size()); le32w(b, data.size()); le16w(b, name.size()); le16w(b, 0);
        put(b, name.data(), name.size()); put(b, data.data(), data.size());
        put(cd, "PK\x01\x02", 4); le16w(cd, 20); le16w(cd, 20); le16w(cd, 0); le16w(cd, 0); le16w(cd, 0); le16w(cd, 0);
        le32w(cd, 0x12345678); le32w(cd, data.size()); le32w(cd, data.size()); le16w(cd, name.size());
        le16w(cd, 0); le16w(cd, 0); le16w(cd, 0); le16w(cd, 0); le32w(cd, 0); le32w(cd, off);
        put(cd, name.data(), name.size());
    }
    uint32_t cdOff = b.size();
    put(b, cd.data(), cd.size());
    put(b, "PK\x05\x06", 4); le16w(b, 0); le16w(b, 0); le16w(b, entries.size()); le16w(b, entries.size());
    le32w(b, cd.size()); le32w(b, cdOff); le16w(b, comment.size()); put(b, comment.data(), comment.size());
    return b;
}

static Bytes makeGif() {
    Bytes b; put(b, "GIF89a", 6); le16w(b, 10); le16w(b, 10); b.push_back(0x81); b.push_back(0); b.push_back(0);
    Bytes gct = rnd(12); put(b, gct.data(), 12);
    b.insert(b.end(), {0x21, 0xF9, 4, 0, 0, 0, 0, 0});            // graphic control ext
    b.insert(b.end(), {0x2C, 0, 0, 0, 0, 10, 0, 10, 0, 0, 2});    // image descriptor + LZW min size
    Bytes d1 = rnd(50); b.push_back(50); put(b, d1.data(), 50);
    Bytes d2 = rnd(20); b.push_back(20); put(b, d2.data(), 20); b.push_back(0);
    b.insert(b.end(), {0x21, 0xFE, 3, 'a', 'b', 'c', 0});          // comment ext
    b.push_back(0x3B);
    return b;
}

static Bytes readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str(); return Bytes(s.begin(), s.end());
}

static int checkDevicePaths() {
    struct { const char* p; bool ok; } cases[] = {
        {"/dev/block/mmcblk1p1", true}, {"/dev/block/vold/public:179,1", true}, {"/dev/block/dm-3", true},
        {"/dev/block/../../etc/passwd", false}, {"/dev/block/a b", false}, {"/dev/block/a;reboot", false},
        {"/dev/block/a$(id)", false}, {"/dev/block/a\"b", false}, {"/sdcard/x", false}, {"", false},
    };
    int bad = 0;
    for (auto& c : cases)
        if (carver::safeDevicePath(c.p) != c.ok) { std::cout << "FAIL devicePath: " << c.p << "\n"; ++bad; }
    return bad;
}

int main() {
    std::vector<std::pair<std::string, Bytes>> files = {
        {"jpg+thumb", makeJpeg(true)}, {"jpg", makeJpeg(false)}, {"png", makePng()}, {"pdf", makePdf()},
        {"webp", makeWebp()}, {"mp4", makeMp4()}, {"jpg2", makeJpeg(true)}, {"png2", makePng()},
        {"mp4b", makeMp4()}, {"pdf2", makePdf()},
        {"mp4-big-moov-last", makeMp4Big(false, 200000)}, {"mp4-big-moov-first", makeMp4Big(true, 150000)},
        {"mp4-multi-box", makeMp4Multi()}, {"jpg3", makeJpeg(true)},
        {"gif", makeGif()}, {"gif2", makeGif()},
        {"zip", makeZip({{"a.txt", rnd(400)}, {"b.bin", rnd(700)}}, "")},
        {"docx+nested", makeZip({{"[Content_Types].xml", rnd(300)}, {"word/document.xml", rnd(600)},
                                 {"word/embeddings/inner.zip", makeZip({{"x.txt", rnd(250)}}, "")}}, "hello")},
        {"apk", makeZip({{"AndroidManifest.xml", rnd(300)}, {"classes.dex", rnd(900)}}, "")},
    };
    // صورة قرص: بيانات عشوائية كثيفة بـ FF و 00 بين الملفات، وآخر ملف قرب النهاية تماماً
    Bytes image;
    for (auto& [name, data] : files) {
        Bytes junk = rnd(rng() % 9000 + 3000);
        for (size_t i = 0; i < junk.size(); i += 7) junk[i] = (rng() & 1) ? 0xFF : 0x00;
        put(image, junk.data(), junk.size());
        put(image, data.data(), data.size());
        if (name == "png") { Bytes cp = makeCorruptPng(); put(image, cp.data(), cp.size()); }
        if (name == "gif2") { Bytes tv = makeTruncVideo(); put(image, tv.data(), tv.size()); }
    }
    // ملف مقطوع عمداً (يجب ألا يُستخرج): jpeg بلا نهاية
    if (carver::kMaxFileSize >= 262144) {  // التجزئة تحتاج نافذة بحث أكبر من الذاكرة الصغيرة المستخدمة في بقية الاختبارات
        Bytes pngA = makeBigPng(3000, 3000, 14);      // الحد بين القطع داخل كتلة IDAT
        Bytes pngB = makeBigPng(4051, 4084, 8);       // كل كتلة = 4096 بالضبط: الحد عند أطراف الكتل
        Bytes jpgGap = makeJpeg(false);               // ملف سليم يقع داخل فجوة pngA
        putFragmented(image, pngA, &jpgGap);
        putFragmented(image, pngB, nullptr);
        putFragmentedMissing(image, makeBigPng(3000, 3000, 10));
        files.push_back({"png-frag", pngA});
        files.push_back({"jpg-in-gap", jpgGap});
        files.push_back({"png-frag-boundary", pngB});
    }
    Bytes trunc = makeJpeg(false); trunc.resize(trunc.size() - 200);
    put(image, trunc.data(), trunc.size());
    Bytes tail = rnd(40); put(image, tail.data(), 40);

    (void)!system("rm -rf /tmp/carve_in /tmp/carve_out && mkdir -p /tmp/carve_in /tmp/carve_out");
    { std::ofstream f("/tmp/carve_in/disk.img", std::ios::binary); f.write((char*)image.data(), image.size()); }
    if (getenv("CARVER_FUZZ")) {  // تشويش: تلف عشوائي وبتر ودمج ترويسات؛ المطلوب ألا ينهار المحرك أو يعلق
        int iters = atoi(getenv("CARVER_FUZZ"));
        long total = 0;
        for (int it = 0; it < iters; ++it) {
            Bytes m = image;
            int flips = rng() % 80 + 1;
            for (int k = 0; k < flips; ++k) m[rng() % m.size()] = rng() & 255;
            for (int k = 0; k < 5; ++k) {  // ترويسة سليمة مزروعة في مكان عشوائي
                auto& f = files[rng() % files.size()].second;
                size_t n = std::min<size_t>(f.size(), rng() % 120 + 8), at = rng() % (m.size() - n);
                std::copy(f.begin(), f.begin() + n, m.begin() + at);
            }
            if (rng() % 3 == 0) m.resize(m.size() - rng() % (m.size() / 2));
            { std::ofstream f("/tmp/carve_in/fuzz.img", std::ios::binary); f.write((char*)m.data(), m.size()); }
            (void)!system("rm -rf /tmp/fuzz_out && mkdir -p /tmp/fuzz_out");
            FILE* ff = fopen("/tmp/carve_in/fuzz.img", "rb");
            total += carver::carveFd(fileno(ff), "/tmp/fuzz_out"); fclose(ff);
        }
        std::cout << "fuzz: " << iters << " iterations OK, files carved in total: " << total << "\n";
        return 0;
    }
    std::cout << "image size: " << image.size() << " bytes, buffer: " << carver::kBufSize << "\n";

    // 1) من أنبوب (كما يعمل وضع root مع dd)، 2) من ملف مباشر
    int failures = checkDevicePaths();
    for (int mode = 0; mode < 2; ++mode) {
        (void)!system("rm -rf /tmp/carve_out && mkdir -p /tmp/carve_out");
        int n;
        if (mode == 0) {
            FILE* pp = popen("cat /tmp/carve_in/disk.img", "r");
            n = carver::carveFd(fileno(pp), "/tmp/carve_out"); pclose(pp);
        } else {
            FILE* ff = fopen("/tmp/carve_in/disk.img", "rb");
            n = carver::carveFd(fileno(ff), "/tmp/carve_out"); fclose(ff);
        }
        std::map<Bytes, std::string> outs;
        DIR* d = opendir("/tmp/carve_out");
        while (auto* e = readdir(d)) if (e->d_name[0] != '.') outs[readFile(std::string("/tmp/carve_out/") + e->d_name)] = std::string(e->d_name).substr(std::string(e->d_name).rfind('.') + 1);
        closedir(d);
        std::cout << (mode ? "file" : "pipe") << " mode: carved " << n << " files\n";
        for (auto& [name, data] : files) {
            static const std::map<std::string, std::string> wantExt = {
                {"gif", "gif"}, {"zip", "zip"}, {"docx+nested", "docx"}, {"apk", "apk"}, {"mp4", "mp4"}, {"pdf", "pdf"}};
            bool ok = outs.count(data) > 0;
            if (ok && wantExt.count(name) && outs[data] != wantExt.at(name)) { std::cout << "  wrong ext for " << name << ": " << outs[data] << "\n"; ok = false; }
            std::cout << "  " << (ok ? "OK   " : "FAIL ") << name << " (" << data.size() << " bytes)\n";
            if (!ok) ++failures;
        }
        if (n != (int)files.size()) { std::cout << "  FAIL: expected " << files.size() << " files, got " << n << "\n"; ++failures; }
    }
    std::cout << (failures ? "FAILED\n" : "ALL PASSED\n");
    return failures ? 1 : 0;
}
