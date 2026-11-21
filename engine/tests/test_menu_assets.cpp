// SPDX-License-Identifier: GPL-3.0-or-later
#include "../tools/engine/menu_assets.h"
#include <zlib.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

using namespace menu_assets;

Bytes operator+(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

namespace {
void Le(Bytes& b, std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(std::uint8_t(v >> (8 * i)));
}
Bytes Ascii(const std::string& s) { return Bytes(s.begin(), s.end()); }
Bytes Wide(const std::u16string& s) {
    Bytes b;
    for (char16_t c : s) Le(b, c, 2);
    return b;
}
std::size_t Count(const Bytes& b, const std::string& what) {
    std::size_t n = 0;
    for (auto it = b.begin(); (it = std::search(it, b.end(), what.begin(), what.end())) != b.end(); ++it) ++n;
    return n;
}

Binder Bundle(const Fmg& fmg) {
    Binder b;
    b.header.assign(0x40, 0);
    std::memcpy(b.header.data(), "BND4", 4);
    b.header[0x20] = 0x24;
    b.header[0x30] = 1;
    b.entry_bytes = 0x24;
    b.wide_names = true;
    for (std::uint32_t id : {200u, 201u, 78u, 204u, 30u, 999u}) {
        BinderFile f;
        f.flags = 0xffffffff00000040ull;
        f.id = id;
        f.name = Wide(u"N:\\msg\\测试" + std::u16string(1, char16_t(u'0' + id % 10)) + u".fmg");
        f.data = FmgWrite(fmg);
        b.files.push_back(f);
    }
    return b;
}

Bytes Packed(const Bytes& data) {
    Bytes h(0x4c, 0);
    std::memcpy(h.data(), "DCX\0", 4);
    std::memcpy(h.data() + 0x18, "DCS\0", 4);
    std::memcpy(h.data() + 0x24, "DCP\0DFLT", 8);
    std::memcpy(h.data() + 0x44, "DCA\0", 4);
    h[0x4b] = 8;
    return DcxWrite(menu_assets::Dcx{h, data});
}

Bytes Tag(int code, const Bytes& body) {
    Bytes t;
    if (body.size() < 0x3f) Le(t, code << 6 | body.size(), 2);
    else Le(t, code << 6 | 0x3f, 2), Le(t, body.size(), 4);
    t.insert(t.end(), body.begin(), body.end());
    return t;
}
Bytes Translate(int tx, int ty) {
    Bytes b(5, 0);
    std::uint64_t bits = 0;
    int at = 0;
    auto put = [&](std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i, ++at) bits |= (v >> i & 1) << (39 - at);
    };
    put(0, 2), put(16, 5), put(std::uint16_t(tx), 16), put(std::uint16_t(ty), 16);
    for (int i = 0; i < 5; ++i) b[i] = std::uint8_t(bits >> (32 - 8 * i));
    return b;
}
Bytes Place(int depth, int character, int tx, int ty, const std::string& name) {
    Bytes b{std::uint8_t((character ? 0x02 : 0x01) | 0x04 | (name.empty() ? 0 : 0x20))};
    Le(b, depth, 2);
    if (character) Le(b, character, 2);
    const Bytes m = Translate(tx, ty);
    b.insert(b.end(), m.begin(), m.end());
    if (!name.empty()) b.insert(b.end(), name.begin(), name.end()), b.push_back(0);
    return Tag(26, b);
}
Bytes Sprite(int id, int frames, const Bytes& tags) {
    Bytes b;
    Le(b, id, 2);
    Le(b, frames, 2);
    b.insert(b.end(), tags.begin(), tags.end());
    b.insert(b.end(), {0, 0});
    return Tag(39, b);
}
Bytes Movie() {
    const Bytes tags = Tag(2, {1, 0, 0});
    Bytes title = Place(1, 1, 40, 40, "StaticText_113020") + Tag(1, {});
    Bytes donor;
    for (int r = 0; r < 6; ++r) donor = donor + Place(56 + 9 * r, 1, 1840, 1060 + 1143 * r, "Item_" + std::to_string(r) + "_0");
    donor = donor + Place(110, 1, 10820, 9415, "Item_6_0");
    Bytes p3{0x06, 0x00, 117, 0, 2, 0};
    const Bytes m = Translate(8937, 980);
    p3.insert(p3.end(), m.begin(), m.end());
    donor = donor + Tag(70, p3) + Tag(1, {});
    for (int r = 0; r < 6; ++r) donor = donor + Place(56 + 9 * r, 0, 1840, 1080 + 1143 * r, "");
    donor = donor + Place(110, 0, 10820, 9435, "") + Tag(1, {});
    Bytes top;
    for (int r = 0; r < 5; ++r) top = top + Place(31 + 7 * r, 1, 1481, -20 + 1353 * r, "Item_" + std::to_string(r) + "_0");
    top = top + Tag(1, {});
    Bytes list = Place(59, 1, 6211, 361, "ScrollBarV") + Tag(1, {});
    Bytes classes;
    Le(classes, 1, 2), Le(classes, 3, 2);
    const std::string cls = "OptionSetting_fla._ControllSetting";
    classes.insert(classes.end(), cls.begin(), cls.end()), classes.push_back(0);
    Bytes body = tags + Sprite(2, 1, title) + Sprite(3, 2, donor) + Sprite(4, 1, list) + Sprite(5, 1, top) +
                 Place(382, 3, 10046, 4134, "ControllSetting") + Place(500, 5, 13754, 6089, "Top") + Tag(76, classes) +
                 Tag(1, {}) + Tag(0, {});
    Bytes head = Ascii("GFX");
    head.push_back(11);
    Le(head, 0, 4);
    head.insert(head.end(), {0x08, 0x00});
    Le(head, 30 << 8, 2), Le(head, 1, 2);
    Bytes all = head + body;
    for (int i = 0; i < 4; ++i) all[4 + i] = std::uint8_t(all.size() >> (8 * i));
    return all;
}
}

int main(int argc, char** argv) {
    const auto english = PcMessages(), chinese = PcMessages(true), russian = PcMessages(Language::Russian);
    assert(english.size() == chinese.size() && english.size() == russian.size() && english.size() > 150);
    assert(LanguageOf("rusru") == Language::Russian && LanguageOf("zhocn") == Language::Chinese &&
           LanguageOf("enggb") == Language::English);
    assert(russian.at(110010).text == "Эффекты (ПК)" && russian.at(110015).help == "Закрыть игру и вернуться на рабочий стол");
    assert(russian.at(119100).text == "Вперёд" && russian.at(119215).text.rfind(". Enter", 0) == 0);
    assert(russian.at(920000).text.find('\n') != std::string::npos);
    assert(russian.at(117010).text == "1280x720" && russian.at(116030).text == "30");  // untranslated rows stay English
    assert(russian.at(116020).help.empty());
    std::size_t translated = 0;
    for (const auto& [id, m] : russian) translated += m.text != english.at(id).text;
    assert(translated > 120);  // the table is translated, not copied from English
    assert(english.at(110010).text == "PC Effects" && chinese.at(110010).text == "PC 特效");
    assert(chinese.at(110015).help == "关闭游戏并返回桌面。");
    assert(chinese.at(117010).text == "1280x720");
    assert(english.at(920000).text.find('\n') != std::string::npos);
    assert(chinese.at(119100).text == "向前移动" && chinese.at(119129).text == "闪避" &&
           chinese.at(119132).text == "调试菜单" && chinese.at(119204).text == "其他");
    assert(english.at(119215).text.front() == ' ');
    assert(KindOf(110006) == Kind::Menu && KindOf(920001) == Kind::Question && KindOf(14000336) == Kind::Event);
    assert(Utf16("a😀") == std::u16string(u"a\U0001F600") && Utf16("한글") == u"한글");

    Fmg fmg;
    fmg.header.assign(0x28, 0);
    fmg.header[2] = 2, fmg.header[8] = 1, fmg.header[0x14] = 0xff;
    fmg.text = {{42, u"原有文字 한글 😀"}, {43, std::nullopt}, {44, u"A가"}, {90, u"A가"}};
    const Bytes fmg_bytes = FmgWrite(fmg);
    const Fmg back = FmgRead(fmg_bytes);
    assert(back.text == fmg.text && back.header[0x14] == 0xff && FmgWrite(back) == fmg_bytes);
    assert(fmg_bytes[0x0c] == 2 && fmg_bytes[0x10] == 4);

    const Binder bundle = Bundle(fmg);
    const Bytes packed = BinderWrite(bundle);
    const Binder reread = BinderRead(packed);
    assert(reread.files.size() == 6 && reread.wide_names);
    for (std::size_t i = 0; i < 6; ++i) {
        const std::size_t e = 0x40 + i * 0x24;
        assert((packed[e + 0x18] & 15) == 0 && reread.files[i].name == bundle.files[i].name &&
               reread.files[i].data == bundle.files[i].data && reread.files[i].flags == bundle.files[i].flags);
    }
    assert(BinderWrite(reread) == packed);

    const Bytes dcx = Packed(packed);
    assert(DcxRead(dcx).data == packed);
    Bytes short_dcx = dcx;
    short_dcx[0x1f] ^= 1;
    bool refused = false;
    try { DcxRead(short_dcx); } catch (const Error&) { refused = true; }
    assert(refused);

    for (const Language language : {Language::English, Language::Chinese, Language::Russian}) {
        for (const bool in_dcx : {false, true}) {
            const Bytes made = MenuMessages(in_dcx ? dcx : packed, language);
            const Binder out = BinderRead(in_dcx ? DcxRead(made).data : made);
            assert(out.files.size() == 6);
            const auto& expect = language == Language::Chinese ? chinese : language == Language::Russian ? russian : english;
            for (const BinderFile& f : out.files) {
                if (f.id == 999) {
                    assert(f.data == bundle.files.back().data);
                    continue;
                }
                const Fmg got = FmgRead(f.data);
                assert(got.text.at(42) == fmg.text.at(42) && !got.text.at(43) && got.text.at(90) == u"A가");
                const Kind kind = f.id == 30 ? Kind::Event : (f.id == 78 || f.id == 204) ? Kind::Question : Kind::Menu;
                for (const auto& [id, m] : expect) {
                    if (KindOf(id) != kind) {
                        assert(!got.text.count(id));
                        continue;
                    }
                    const std::string& s = f.id == 201 ? m.help : m.text;
                    assert(got.text.at(id) == (s.empty() ? std::nullopt : std::optional<std::u16string>(Utf16(s))));
                }
            }
        }
    }
    Fmg taken = fmg;
    taken.text[110006] = u"mine";
    refused = false;
    try { MenuMessages(BinderWrite(Bundle(taken))); } catch (const Error&) { refused = true; }
    assert(refused);

    const Bytes movie = OptionMovie(Movie());
    for (const char* name : {"PCGraphics", "PCControls", "PCKeys", "PCEffects", "PCCamera", "PCEnhance", "PCColour", "PCLighting", "PCDetail", "PCLooks"})
        assert(Count(movie, std::string(name) + '\0') == 1);
    for (const char* title : {"117020", "118020", "119020", "120020", "121020", "124020", "125020", "126020", "127020", "128020"})
        assert(Count(movie, std::string("StaticText_") + title) == 1);
    assert(Count(movie, "StaticText_113020") == 1);
    assert(Count(movie, "Item_7_0") == 4);
    assert(Count(movie, "Item_6_0") == 1 + 1 + 4);
    assert(Count(movie, "Item_3_0") == 1 + 1 + 9);
    assert(Count(movie, "ScrollBarV") == 2);
    assert(Count(movie, "OptionSetting_fla._ControllSetting") == 11);
    assert(movie[4] == (movie.size() & 0xff) && movie[5] == (movie.size() >> 8 & 0xff));
    refused = false;
    try { OptionMovie(movie); } catch (const Error&) { refused = true; }
    assert(refused);

    if (argc > 1) {
        std::ifstream in(argv[1], std::ios::binary);
        const Bytes dump((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const Bytes made = OptionMovie(dump);
        assert(made.size() > dump.size() && Count(made, "StaticText_119020") == 1 && Count(made, "ScrollBarV") == 2);
        std::printf("the dump's movie: %zu -> %zu bytes\n", dump.size(), made.size());
    }
    std::puts("PASS: menu assets: PC strings, FMG, BND4, DCX, messages in every group, the option movie");
}
