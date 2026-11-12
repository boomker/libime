/*
 * SPDX-FileCopyrightText: 2017-2017 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <fcitx-utils/log.h>
#include "libime/core/historybigram.h"
#include "libime/core/lattice.h"
#include "libime/core/userlanguagemodel.h"
#include "libime/pinyin/pinyincontext.h"
#include "libime/pinyin/pinyincorrectionprofile.h"
#include "libime/pinyin/pinyindictionary.h"
#include "libime/pinyin/pinyinencoder.h"
#include "libime/pinyin/pinyinime.h"
#include "libime/pinyin/shuangpinprofile.h"
#include "testdir.h"

using namespace libime;

namespace {

size_t candidateIndex(PinyinContext &c, const std::string &candidate) {
    auto iter =
        std::ranges::find(c.candidates(), candidate, &SentenceResult::toString);
    FCITX_ASSERT(iter != c.candidates().end());
    return std::distance(c.candidates().begin(), iter);
}

void selectCandidate(PinyinContext &c, const std::string &candidate) {
    c.select(candidateIndex(c, candidate));
}

void testPinyin(PinyinIME &ime) {
    PinyinContext c(&ime);

    c.type("nihaozhongguo");
    FCITX_ASSERT(c.candidates().size() == c.candidateSet().size());
    FCITX_ASSERT(c.candidateSet().count("你好中国"));
    c.setCursor(5);
    FCITX_ASSERT(c.candidates().size() == c.candidateSet().size());
    FCITX_ASSERT(c.candidatesToCursor().size() ==
                 c.candidatesToCursorSet().size());
    FCITX_ASSERT(c.candidates().size() != c.candidatesToCursor().size())
        << c.candidatesToCursorSet();
    FCITX_ASSERT(!c.candidatesToCursorSet().count("你好中国"));
    FCITX_ASSERT(c.candidatesToCursorSet().count("你好"));
    c.setCursor(0);
    auto iter = std::ranges::find(c.candidates(), "你好中国",
                                  &SentenceResult::toString);
    FCITX_ASSERT(iter != c.candidates().end());
    FCITX_ASSERT(!ime.dict()->lookupWord(PinyinDictionary::UserDict,
                                         "ni'hao'zhong'guo", "你好中国"));
    c.select(std::distance(c.candidates().begin(), iter));
    c.learn();
    FCITX_ASSERT(ime.model()->history().containsBigram("你", "好"));
    FCITX_ASSERT(ime.model()->history().containsBigram("好", "中国"));
}

void testShuangpin(PinyinIME &ime) {
    PinyinContext c(&ime);

    c.setUseShuangpin(true);

    c.type("bkqilb");
    FCITX_ASSERT(c.candidates().size() == c.candidateSet().size());
    FCITX_ASSERT(c.candidateSet().count("冰淇淋"));
    c.clear();

    c.type("bkqiln");
    FCITX_ASSERT(c.candidates().size() == c.candidateSet().size());
    FCITX_ASSERT(!c.candidateSet().contains("冰淇淋"));
    c.clear();

    ime.setCorrectionProfile(std::make_shared<PinyinCorrectionProfile>(
        BuiltinPinyinCorrectionProfile::Qwerty));
    ime.setShuangpinProfile(std::make_shared<libime::ShuangpinProfile>(
        ShuangpinBuiltinProfile::Xiaohe, ime.correctionProfile().get()));
    ime.setFuzzyFlags({PinyinFuzzyFlag::Inner, PinyinFuzzyFlag::Correction});

    c.type("bkqiln");
    FCITX_ASSERT(c.candidates().size() == c.candidateSet().size());
    FCITX_ASSERT(c.candidateSet().contains("冰淇淋"));
    c.clear();
}

void testHistory(PinyinIME &ime) {
    PinyinContext c(&ime);
    c.type("kuai");
    FCITX_ASSERT(c.candidateSet().contains("会"));
    auto kuaiIndex = candidateIndex(c, "会");
    c.clear();
    c.type("hui");
    FCITX_ASSERT(c.candidateSet().contains("会"));
    selectCandidate(c, "会");
    FCITX_ASSERT(c.selected());
    c.learn();
    c.clear();
    c.type("kuai");
    auto kuaiIndexNew = candidateIndex(c, "会");
    FCITX_ASSERT(kuaiIndexNew == kuaiIndex);
}

void testGroupedShuangpin(PinyinIME &ime) {
    ime.setFuzzyFlags(PinyinFuzzyFlag::None);
    auto ordinaryProfile =
        std::make_shared<ShuangpinProfile>(ShuangpinBuiltinProfile::Xiaohe);
    ime.setShuangpinProfile(ordinaryProfile);
    std::unordered_map<char, char> mapping;
    for (const std::string group :
         {"q", "we", "rt", "y", "u", "io", "p", "a", "sd", "fg", "h", "jk",
          "l;", "z", "xc", "v", "bn", "m"}) {
        for (char key : group) {
            mapping.emplace(key, group.front());
        }
    }
    auto groupedProfile =
        std::make_shared<ShuangpinProfile>(*ordinaryProfile, mapping);

    PinyinContext ordinary(&ime);
    ordinary.setUseShuangpin(true);
    ordinary.type("nihc");
    FCITX_ASSERT(ordinary.candidateSet().contains("你好"));
    const auto ordinaryCandidates = ordinary.candidateSet();

    PinyinContext grouped(&ime);
    grouped.setUseShuangpin(true);
    grouped.setShuangpinProfile(groupedProfile);
    grouped.type("bihx");
    FCITX_ASSERT(grouped.candidateSet().contains("你好"));
    auto index = candidateIndex(grouped, "你好");
    FCITX_ASSERT(grouped.candidateFullPinyin(index) == "ni'hao");
    const auto groupedCandidates = grouped.candidateSet();
    grouped.setShuangpinProfile(groupedProfile);
    FCITX_ASSERT(grouped.userInput() == "bihx");
    FCITX_ASSERT(grouped.candidateSet() == groupedCandidates);
    FCITX_ASSERT(ordinary.userInput() == "nihc");
    FCITX_ASSERT(ordinary.candidateSet() == ordinaryCandidates);
    FCITX_ASSERT(ime.shuangpinProfile() == ordinaryProfile);
    FCITX_ASSERT(ordinary.shuangpinProfile() == ordinaryProfile);

    auto secondGroupedProfile =
        std::make_shared<ShuangpinProfile>(*ordinaryProfile, mapping);
    PinyinContext secondGrouped(&ime);
    secondGrouped.setUseShuangpin(true);
    secondGrouped.setShuangpinProfile(secondGroupedProfile);
    secondGrouped.type("bihx");
    FCITX_ASSERT(secondGrouped.candidateSet().contains("你好"));
    grouped.setShuangpinProfile(secondGroupedProfile);
    FCITX_ASSERT(grouped.empty());
    FCITX_ASSERT(secondGrouped.userInput() == "bihx");
    FCITX_ASSERT(ordinary.userInput() == "nihc");

    grouped.setShuangpinProfile(nullptr);
    FCITX_ASSERT(grouped.shuangpinProfile() == ordinaryProfile);
    grouped.type("nihc");
    FCITX_ASSERT(grouped.candidateSet().contains("你好"));
}

void testGroupedShuangpinWithCorrection(PinyinIME &ime) {
    auto correction = std::make_shared<PinyinCorrectionProfile>(
        BuiltinPinyinCorrectionProfile::Qwerty);
    ime.setCorrectionProfile(correction);
    ime.setFuzzyFlags(PinyinFuzzyFlag::Correction);
    std::unordered_map<char, char> mapping;
    for (const std::string group :
         {"q", "we", "rt", "y", "u", "io", "p", "a", "sd", "fg", "h", "jk",
          "l;", "z", "xc", "v", "bn", "m"}) {
        for (char key : group) {
            mapping.emplace(key, group.front());
        }
    }
    for (const auto &[builtin, input] :
         {std::pair{ShuangpinBuiltinProfile::Xiaohe, "bihx"},
          std::pair{ShuangpinBuiltinProfile::MS, "bihj"},
          std::pair{ShuangpinBuiltinProfile::Ziranma, "bihj"}}) {
        auto source =
            std::make_shared<ShuangpinProfile>(builtin, correction.get());
        ime.setShuangpinProfile(source);
        PinyinContext grouped(&ime);
        grouped.setUseShuangpin(true);
        grouped.setShuangpinProfile(
            std::make_shared<ShuangpinProfile>(*source, mapping));
        for (char key : std::string(input)) {
            grouped.type(std::string(1, key));
        }
        FCITX_ASSERT(grouped.userInput() == input);
        FCITX_ASSERT(grouped.candidateSet().contains("你好"));
        const auto index = candidateIndex(grouped, "你好");
        FCITX_ASSERT(grouped.candidateFullPinyin(index) == "ni'hao");
        grouped.select(index);
        FCITX_ASSERT(grouped.selected());
        FCITX_ASSERT(grouped.sentence() == "你好");
    }
}

void testGroupedSyllableConfirmation(PinyinIME &ime) {
    ime.setFuzzyFlags(PinyinFuzzyFlag::None);
    auto source =
        std::make_shared<ShuangpinProfile>(ShuangpinBuiltinProfile::Xiaohe);
    ime.setShuangpinProfile(source);
    std::unordered_map<char, char> mapping;
    for (const std::string group :
         {"q", "we", "rt", "y", "u", "io", "p", "a", "sd", "fg", "h", "jk",
          "l;", "z", "xc", "v", "bn", "m"}) {
        for (char key : group) {
            mapping.emplace(key, group.front());
        }
    }
    const auto profile = std::make_shared<ShuangpinProfile>(*source, mapping);
    const PinyinSyllable bi(PinyinInitial::B, PinyinFinal::I);
    const PinyinSyllable ni(PinyinInitial::N, PinyinFinal::I);
    const PinyinSyllable hao(PinyinInitial::H, PinyinFinal::AO);
    const PinyinSyllable hua(PinyinInitial::H, PinyinFinal::UA);
    ime.dict()->addWord(PinyinDictionary::UserDict, "bi'hao", "比好");
    ime.dict()->addWord(PinyinDictionary::UserDict, "bi'hua", "比划");
    ime.dict()->addWord(PinyinDictionary::UserDict, "bi'ni", "比你");
    ime.dict()->addWord(PinyinDictionary::UserDict, "ni'ni", "你你");

    PinyinContext c(&ime);
    c.setUseShuangpin(true);
    c.setShuangpinProfile(profile);
    c.type("bihx");
    const auto before = c.candidateSet();
    FCITX_ASSERT(before.contains("你好"));
    FCITX_ASSERT(c.nextSyllableOffset() == 0);
    FCITX_ASSERT(!c.selectSyllable(2, 4, hao));
    FCITX_ASSERT(!c.selectSyllable(0, 4, bi));
    FCITX_ASSERT(!c.selectSyllable(0, 2, hao));
    FCITX_ASSERT(c.selectSyllable(0, 2, bi));
    FCITX_ASSERT(c.userInput() == "bihx");
    FCITX_ASSERT(c.nextSyllableOffset() == 2);
    FCITX_ASSERT(!c.candidateSet().contains("你好"));
    FCITX_ASSERT(c.candidateSet().contains("比好"));
    FCITX_ASSERT(c.candidateSet().contains("比划"));
    FCITX_ASSERT(!c.selectSyllable(0, 2, bi));
    FCITX_ASSERT(c.selectSyllable(2, 4, hua));
    FCITX_ASSERT(c.nextSyllableOffset() == 4);
    FCITX_ASSERT(c.candidateSet().contains("比划"));
    FCITX_ASSERT(!c.candidateSet().contains("比好"));
    for (size_t i = 0; i < c.candidates().size(); i++) {
        const auto pinyin = c.candidateFullPinyin(i);
        FCITX_ASSERT(pinyin == "bi" || pinyin == "bi'hua") << pinyin;
    }

    // Newly typed keys are removed before the last confirmation is undone.
    c.type("b");
    FCITX_ASSERT(!c.cancelSyllable());
    c.backspace();
    FCITX_ASSERT(c.userInput() == "bihx");
    FCITX_ASSERT(c.cancelSyllable());
    FCITX_ASSERT(c.nextSyllableOffset() == 2);
    FCITX_ASSERT(c.candidateSet().contains("比好"));
    FCITX_ASSERT(c.cancelSyllable());
    FCITX_ASSERT(c.nextSyllableOffset() == 0);
    FCITX_ASSERT(c.candidateSet() == before);
    FCITX_ASSERT(!c.cancelSyllable());

    // Repeated input keys must not reuse a different position's reading.
    c.clear();
    c.type("bibi");
    FCITX_ASSERT(c.selectSyllable(0, 2, bi));
    FCITX_ASSERT(c.selectSyllable(2, 4, ni));
    FCITX_ASSERT(c.candidateSet().contains("比你"));
    FCITX_ASSERT(c.candidateFullPinyin(candidateIndex(c, "比你")) == "bi'ni");
    c.clear();
    FCITX_ASSERT(c.nextSyllableOffset() == 0);
    c.type("bibi");
    FCITX_ASSERT(c.selectSyllable(0, 2, ni));
    FCITX_ASSERT(c.selectSyllable(2, 4, ni));
    FCITX_ASSERT(c.nextSyllableOffset() == 4);
    FCITX_ASSERT(c.candidateSet().contains("你你"));

    // Moving the cursor preserves constraints; an edit invalidates the
    // intersected confirmation and every confirmation after it.
    c.clear();
    c.type("bihx");
    FCITX_ASSERT(c.selectSyllable(0, 2, bi));
    FCITX_ASSERT(c.selectSyllable(2, 4, hao));
    c.setCursor(3);
    FCITX_ASSERT(c.nextSyllableOffset() == 4);
    FCITX_ASSERT(!c.cancelSyllable());
    c.type("x");
    FCITX_ASSERT(c.nextSyllableOffset() == 2);
    c.erase(1, 2);
    FCITX_ASSERT(c.nextSyllableOffset() == 0);

    // Separators and word selection do not shift the absolute constraints.
    c.clear();
    c.type("bi'hx");
    FCITX_ASSERT(c.selectSyllable(0, 2, bi));
    FCITX_ASSERT(c.nextSyllableOffset() == 3);
    FCITX_ASSERT(c.selectSyllable(3, 5, hua));
    selectCandidate(c, "比");
    FCITX_ASSERT(c.selectedLength() == 2);
    FCITX_ASSERT(c.nextSyllableOffset() == 5);
    FCITX_ASSERT(c.candidateSet().contains("划"));
    FCITX_ASSERT(!c.candidateSet().contains("好"));
    FCITX_ASSERT(c.cancelSyllable());
    FCITX_ASSERT(c.nextSyllableOffset() == 3);
    FCITX_ASSERT(!c.cancelSyllable());
    c.cancel();
    FCITX_ASSERT(c.cancelSyllable());
    FCITX_ASSERT(c.nextSyllableOffset() == 0);

    // Confirming a single initial fixes its boundary before new keys arrive.
    c.clear();
    c.type("b");
    FCITX_ASSERT(c.selectSyllable(
        0, 1, PinyinSyllable(PinyinInitial::B, PinyinFinal::Invalid)));
    c.type("hx");
    FCITX_ASSERT(c.userInput() == "bhx");
    FCITX_ASSERT(c.nextSyllableOffset() == 1);
    FCITX_ASSERT(c.candidateSet().contains("比划"));
    FCITX_ASSERT(c.selectSyllable(1, 3, hua));
    FCITX_ASSERT(c.nextSyllableOffset() == 3);
    FCITX_ASSERT(c.candidateSet().contains("比划"));
    FCITX_ASSERT(c.cancelSyllable());
    FCITX_ASSERT(!c.cancelSyllable());
    c.backspace();
    c.backspace();
    FCITX_ASSERT(c.userInput() == "b");
    FCITX_ASSERT(c.cancelSyllable());

    c.clear();
    c.type("bihx");
    FCITX_ASSERT(c.selectSyllable(0, 2, bi));
    c.setShuangpinProfile(nullptr);
    FCITX_ASSERT(c.empty());
    FCITX_ASSERT(c.nextSyllableOffset() == 0);
    FCITX_ASSERT(!c.cancelSyllable());
}

} // namespace

int main() {
    fcitx::Log::setLogRule("libime=5");
    libime::PinyinIME ime(
        std::make_unique<PinyinDictionary>(),
        std::make_unique<UserLanguageModel>(LIBIME_BINARY_DIR "/data/sc.lm"));
    ime.setNBest(2);
    ime.dict()->load(PinyinDictionary::SystemDict,
                     LIBIME_BINARY_DIR "/data/sc.dict",
                     PinyinDictFormat::Binary);
    PinyinFuzzyFlags flags = PinyinFuzzyFlag::Inner;
    ime.setFuzzyFlags(flags);
    ime.setScoreFilter(1.0F);
    ime.setShuangpinProfile(
        std::make_shared<ShuangpinProfile>(ShuangpinBuiltinProfile::Xiaohe));

    testPinyin(ime);
    testShuangpin(ime);
    testHistory(ime);
    testGroupedShuangpin(ime);
    testGroupedShuangpinWithCorrection(ime);
    testGroupedSyllableConfirmation(ime);

    return 0;
}
