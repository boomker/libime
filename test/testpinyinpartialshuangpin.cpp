/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include <algorithm>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
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

using namespace libime;

namespace {
std::unique_ptr<PinyinIME> makeIME() {
    auto ime =
        std::make_unique<PinyinIME>(std::make_unique<PinyinDictionary>(),
                                    std::make_unique<UserLanguageModel>());
    ime->setShuangpinProfile(
        std::make_shared<ShuangpinProfile>(ShuangpinBuiltinProfile::Xiaohe));
    ime->setFuzzyFlags(PinyinFuzzyFlag::PartialSp);
    ime->setPartialSpLength(3);
    ime->setNBest(2);
    // Exact wu+m wins by score; the highest abbreviation is 为什么.
    ime->dict()->addWord(PinyinDictionary::SystemDict, "wu'ming", "无名", 0);
    ime->dict()->addWord(PinyinDictionary::SystemDict, "wu'mi", "五米", -0.2F);
    ime->dict()->addWord(PinyinDictionary::SystemDict, "wei'shen'me", "为什么",
                         -1);
    ime->dict()->addWord(PinyinDictionary::SystemDict, "wan'shen'me", "玩什么",
                         -2);
    ime->dict()->addWord(PinyinDictionary::SystemDict, "wan'quan", "完全", 0);
    ime->dict()->addWord(PinyinDictionary::SystemDict, "ni'hao", "你好", 0);
    ime->dict()->addWord(PinyinDictionary::SystemDict, "nang", "囊", 0);
    return ime;
}

void expectFirst(PinyinContext &c, const std::string &word) {
    FCITX_ASSERT(!c.candidates().empty());
    FCITX_ASSERT(c.candidates().front().toString() == word)
        << c.candidates().front().toString() << " expected " << word;
    FCITX_ASSERT(c.candidates().size() == c.candidateSet().size());
}

void choose(PinyinContext &c, const std::string &word, bool learn = true) {
    const auto it =
        std::ranges::find(c.candidates(), word, &SentenceResult::toString);
    FCITX_ASSERT(it != c.candidates().end()) << word;
    c.select(std::distance(c.candidates().begin(), it));
    FCITX_ASSERT(c.selected());
    if (learn) {
        c.learn();
    }
    c.clear();
}

void testPromotionAndLearning() {
    auto ime = makeIME();
    PinyinContext c(ime.get());
    c.setUseShuangpin(true);
    c.type("wum");
    expectFirst(c, "为什么");
    FCITX_ASSERT(c.candidateSet().contains("玩什么"));
    // Clicks which are cancelled or not learned do not override preference.
    choose(c, "无名", false);
    c.type("wum");
    choose(c, "无名", false);
    c.type("wum");
    expectFirst(c, "为什么");

    choose(c, "无名");
    c.type("wum");
    expectFirst(c, "为什么");
    choose(c, "无名");
    c.type("wum");
    expectFirst(c, "无名");
    FCITX_ASSERT(c.candidateSet().contains("为什么"));

    // Existing history persistence and forgetting carry the preference.
    std::stringstream history;
    ime->model()->save(history);
    auto restored = makeIME();
    restored->model()->load(history);
    PinyinContext afterRestart(restored.get());
    afterRestart.setUseShuangpin(true);
    afterRestart.type("wum");
    expectFirst(afterRestart, "无名");
    restored->model()->history().forget("无名");
    afterRestart.clear();
    afterRestart.type("wum");
    expectFirst(afterRestart, "为什么");
}

void testThresholdAndModes() {
    auto ime = makeIME();
    PinyinContext c(ime.get());
    c.setUseShuangpin(true);
    ime->setPartialSpLength(4);
    c.type("wum");
    FCITX_ASSERT(!c.candidateSet().contains("为什么"));
    ime->setPartialSpLength(3);
    FCITX_ASSERT(c.empty());
    c.type("wum");
    expectFirst(c, "为什么");
    ime->setPartialSpLength(2);
    c.type("wq");
    FCITX_ASSERT(c.candidateSet().contains("完全"));
    c.clear();
    c.type("nh");
    FCITX_ASSERT(c.candidateSet().contains("你好"));
    expectFirst(c, "囊");
    ime->setFuzzyFlags(PinyinFuzzyFlag::None);
    c.type("wum");
    FCITX_ASSERT(!c.candidateSet().contains("为什么"));
    c.clear();
    c.setUseShuangpin(false);
    ime->setFuzzyFlags(PinyinFuzzyFlag::PartialSp);
    c.type("wuming");
    expectFirst(c, "无名");
}

void testBounds() {
    auto ime = makeIME();
    ime->setPartialSpLength(0);
    FCITX_ASSERT(ime->partialSpLength() == 0);
    ime->setPartialSpLength(1);
    FCITX_ASSERT(ime->partialSpLength() == 1);
    ime->setPartialSpLength(100);
    FCITX_ASSERT(ime->partialSpLength() == 4);
}

void testCandidateLimitAndExplicitBoundaries() {
    auto ime = makeIME();
    ime->setNBest(1);
    ime->setWordCandidateLimit(1);
    PinyinContext c(ime.get());
    c.setUseShuangpin(true);
    c.type("wum");
    expectFirst(c, "为什么");
    FCITX_ASSERT(c.candidateSet().contains("无名"));
    FCITX_ASSERT(c.candidates().size() == 2);

    // Explicit syllable boundaries keep wu+m and its ordinary ranking.
    ime->setWordCandidateLimit(0);
    c.clear();
    c.type("wum");
    FCITX_ASSERT(c.selectSyllable(
        0, 2, PinyinSyllable(PinyinInitial::W, PinyinFinal::U)));
    expectFirst(c, "无名");
    FCITX_ASSERT(!c.candidateSet().contains("为什么"));
    c.clear();
    c.type("wu'm");
    expectFirst(c, "无名");

    // A long-word completion cannot masquerade as a bare abbreviation.
    ime->dict()->addWord(PinyinDictionary::SystemDict, "wei'shen'me'ne",
                         "为什么呢", 3);
    ime->setPartialLongWordLimit(3);
    c.clear();
    c.type("wum");
    expectFirst(c, "为什么");
    FCITX_ASSERT(c.candidateSet().contains("为什么呢"));
}

void testAutomaticInitialFuzzy() {
    const ShuangpinProfile profile(ShuangpinBuiltinProfile::Xiaohe);
    for (const auto &[key, initial, alternate, fuzzy] :
         {std::tuple{'z', PinyinInitial::Z, PinyinInitial::ZH,
                     PinyinFuzzyFlag::Z_ZH},
          std::tuple{'v', PinyinInitial::ZH, PinyinInitial::Z,
                     PinyinFuzzyFlag::Z_ZH},
          std::tuple{'c', PinyinInitial::C, PinyinInitial::CH,
                     PinyinFuzzyFlag::C_CH},
          std::tuple{'i', PinyinInitial::CH, PinyinInitial::C,
                     PinyinFuzzyFlag::C_CH},
          std::tuple{'s', PinyinInitial::S, PinyinInitial::SH,
                     PinyinFuzzyFlag::S_SH},
          std::tuple{'u', PinyinInitial::SH, PinyinInitial::S,
                     PinyinFuzzyFlag::S_SH}}) {
        const auto hasSyllable = [](const auto &syllables,
                                    PinyinInitial initial, PinyinFinal final,
                                    PinyinFuzzyFlags flags) {
            return std::ranges::any_of(syllables, [&](const auto &entry) {
                return entry.first == initial &&
                       std::ranges::any_of(entry.second,
                                           [&](const auto &value) {
                                               return value.first == final &&
                                                      value.second == flags;
                                           });
            });
        };
        const auto partial = PinyinEncoder::shuangpinToSyllablesWithFuzzyFlags(
            std::string(1, key), profile, PinyinFuzzyFlag::None);
        FCITX_ASSERT(hasSyllable(partial, initial, PinyinFinal::Invalid,
                                 PinyinFuzzyFlag::None));
        FCITX_ASSERT(
            hasSyllable(partial, alternate, PinyinFinal::Invalid, fuzzy));

        // Valid two-key syllables still require the explicit fuzzy option.
        const auto full = PinyinEncoder::shuangpinToSyllablesWithFuzzyFlags(
            std::string(1, key) + "a", profile, PinyinFuzzyFlag::None);
        FCITX_ASSERT(
            hasSyllable(full, initial, PinyinFinal::A, PinyinFuzzyFlag::None));
        FCITX_ASSERT(!hasSyllable(full, alternate, PinyinFinal::A, fuzzy));
        const auto enabled = PinyinEncoder::shuangpinToSyllablesWithFuzzyFlags(
            std::string(1, key) + "a", profile, fuzzy);
        FCITX_ASSERT(hasSyllable(enabled, alternate, PinyinFinal::A, fuzzy));

        const auto fullPinyinInitial =
            PinyinEncoder::stringToSyllablesWithFuzzyFlags(
                PinyinEncoder::initialToString(initial), nullptr,
                PinyinFuzzyFlag::None);
        FCITX_ASSERT(hasSyllable(fullPinyinInitial, initial,
                                 PinyinFinal::Invalid, PinyinFuzzyFlag::None));
        FCITX_ASSERT(hasSyllable(fullPinyinInitial, alternate,
                                 PinyinFinal::Invalid, fuzzy));
        const auto fullPinyinSyllable =
            PinyinEncoder::stringToSyllablesWithFuzzyFlags(
                PinyinEncoder::initialToString(initial) + "a", nullptr,
                PinyinFuzzyFlag::None);
        FCITX_ASSERT(hasSyllable(fullPinyinSyllable, initial, PinyinFinal::A,
                                 PinyinFuzzyFlag::None));
        FCITX_ASSERT(
            !hasSyllable(fullPinyinSyllable, alternate, PinyinFinal::A, fuzzy));
    }

    // Exact aliases on a merged keyboard must retain their exact flags, even
    // when an automatic fuzzy version was generated first.
    const ShuangpinProfile merged(profile, {{'z', 'z'},
                                            {'v', 'z'},
                                            {'c', 'c'},
                                            {'i', 'c'},
                                            {'s', 's'},
                                            {'u', 's'}});
    for (const auto &[key, first, second] :
         {std::tuple{'z', PinyinInitial::Z, PinyinInitial::ZH},
          std::tuple{'c', PinyinInitial::C, PinyinInitial::CH},
          std::tuple{'s', PinyinInitial::S, PinyinInitial::SH}}) {
        const auto syllables =
            PinyinEncoder::shuangpinToSyllablesWithFuzzyFlags(
                std::string(1, key), merged, PinyinFuzzyFlag::None);
        for (const auto initial : {first, second}) {
            const auto it = std::ranges::find(
                syllables, initial, &decltype(syllables)::value_type::first);
            FCITX_ASSERT(it != syllables.end());
            FCITX_ASSERT(std::ranges::any_of(it->second, [](const auto &value) {
                return value.first == PinyinFinal::Invalid &&
                       value.second == PinyinFuzzyFlag::None;
            }));
        }
    }

    auto ime = makeIME();
    // Initial fuzzy matching follows the abbreviation switch and threshold,
    // including pairs that are not valid full shuangpin.
    ime->setFuzzyFlags(PinyinFuzzyFlag::PartialSp);
    ime->setPartialSpLength(2);
    PinyinContext c(ime.get());
    c.setUseShuangpin(true);
    ime->dict()->addWord(PinyinDictionary::SystemDict, "she'qu", "社区");
    ime->dict()->addWord(PinyinDictionary::SystemDict, "zheng'que", "正确");
    ime->dict()->addWord(PinyinDictionary::SystemDict, "chu'qu", "出去");
    for (const auto &[input, word] :
         {std::pair{"sq", "社区"}, std::pair{"zq", "正确"},
          std::pair{"cq", "出去"}}) {
        c.type(input);
        FCITX_ASSERT(c.candidateSet().contains(word)) << input;
        c.clear();
    }
    ime->setFuzzyFlags(PinyinFuzzyFlag::None);
    // Xiaohe ws is a valid pair, so wsm still needs the PartialSp graph.
    c.type("wsm");
    FCITX_ASSERT(!c.candidateSet().contains("为什么"));
    c.clear();
    c.type("wum");
    FCITX_ASSERT(!c.candidateSet().contains("为什么"));

    // Enabling shorter partial matching still promotes the best abbreviation.
    ime->setPartialSpLength(3);
    ime->setFuzzyFlags(PinyinFuzzyFlag::PartialSp);
    c.type("wsm");
    expectFirst(c, "为什么");
    FCITX_ASSERT(c.candidateSet().contains("玩什么"));
    FCITX_ASSERT(!ime->fuzzyFlags().test(PinyinFuzzyFlag::S_SH));
    c.clear();
    c.type("wum");
    expectFirst(c, "为什么");

    ime->setFuzzyFlags(PinyinFuzzyFlag::None);
    c.setUseShuangpin(false);
    c.type("wsm");
    FCITX_ASSERT(c.candidateSet().contains("为什么"));
    c.clear();
    ime->dict()->addWord(PinyinDictionary::SystemDict, "wo'sao'miao", "我扫描");
    c.type("wshm");
    FCITX_ASSERT(c.candidateSet().contains("我扫描"));
    c.clear();
    ime->dict()->addWord(PinyinDictionary::SystemDict, "zi", "资");
    ime->dict()->addWord(PinyinDictionary::SystemDict, "zhi", "支");
    ime->setPartialSpLength(2);
    c.type("zh");
    FCITX_ASSERT(c.candidateSet().contains("资"));
    FCITX_ASSERT(c.candidateSet().contains("支"));
    c.clear();
    c.type("zhi");
    FCITX_ASSERT(c.candidateSet().contains("支"));
    FCITX_ASSERT(!c.candidateSet().contains("资"));
}
void testContextCorrectionRestriction() {
    auto ime = makeIME();
    const auto correction = std::make_shared<PinyinCorrectionProfile>(
        BuiltinPinyinCorrectionProfile::Qwerty);
    ime->setCorrectionProfile(correction);
    ime->setShuangpinProfile(std::make_shared<ShuangpinProfile>(
        ShuangpinBuiltinProfile::Xiaohe, correction.get()));
    ime->setFuzzyFlags({PinyinFuzzyFlag::Correction, PinyinFuzzyFlag::S_SH});
    ime->dict()->addWord(PinyinDictionary::SystemDict, "ni", "你");

    PinyinContext ordinary(ime.get());
    PinyinContext compact(ime.get());
    ordinary.setUseShuangpin(true);
    compact.setUseShuangpin(true);
    compact.setCorrectionEnabled(false);
    ordinary.type("mi");
    compact.type("mi");
    FCITX_ASSERT(ordinary.candidateSet().contains("你"));
    FCITX_ASSERT(!compact.candidateSet().contains("你"));
    FCITX_ASSERT(!compact.selectSyllable(
        0, 2, PinyinSyllable(PinyinInitial::N, PinyinFinal::I)));
    FCITX_ASSERT(ime->fuzzyFlags().test(PinyinFuzzyFlag::Correction));
    FCITX_ASSERT(compact.fuzzyFlags().test(PinyinFuzzyFlag::S_SH));

    // Restriction changes invalidate graph/match caches and retain typed keys.
    compact.setCorrectionEnabled(true);
    FCITX_ASSERT(compact.userInput() == "mi");
    FCITX_ASSERT(compact.candidateSet().contains("你"));
    compact.setCorrectionEnabled(false);
    FCITX_ASSERT(compact.userInput() == "mi");
    FCITX_ASSERT(!compact.candidateSet().contains("你"));
    FCITX_ASSERT(ordinary.candidateSet().contains("你"));

    // Reapplying the shared config cannot reactivate restricted correction.
    ime->setFuzzyFlags({PinyinFuzzyFlag::Correction, PinyinFuzzyFlag::S_SH});
    ordinary.type("mi");
    compact.type("mi");
    FCITX_ASSERT(ordinary.candidateSet().contains("你"));
    FCITX_ASSERT(!compact.candidateSet().contains("你"));

    ordinary.clear();
    compact.clear();
    ordinary.setUseShuangpin(false);
    compact.setUseShuangpin(false);
    ordinary.type("mi");
    compact.type("mi");
    FCITX_ASSERT(ordinary.candidateSet().contains("你"));
    FCITX_ASSERT(!compact.candidateSet().contains("你"));
    compact.clear();
    compact.type("ni");
    FCITX_ASSERT(compact.candidateSet().contains("你"));

    // Returning to a permitted layout respects a disabled saved option.
    ime->setFuzzyFlags(PinyinFuzzyFlag::S_SH);
    compact.setCorrectionEnabled(true);
    compact.type("mi");
    FCITX_ASSERT(!compact.candidateSet().contains("你"));
}
void testAbbreviationThresholds() {
    for (const bool shuangpin : {false, true}) {
        auto ime = makeIME();
        ime->dict()->addWord(PinyinDictionary::SystemDict, "ni", "你", 0);
        ime->dict()->addWord(PinyinDictionary::SystemDict, "hao", "好", 0);
        ime->dict()->addWord(PinyinDictionary::SystemDict, "ni'hao'ma", "你好吗", 0);
        ime->dict()->addWord(PinyinDictionary::SystemDict, "ni'hao'ma'ne", "你好吗呢", 0);
        PinyinContext c(ime.get());
        c.setUseShuangpin(shuangpin);
        for (size_t minimum = 0; minimum <= 4; minimum++) {
            ime->setPartialSpLength(minimum);
            for (const auto &[keys, word] :
                 {std::pair{"nh", "你好"}, {"nhm", "你好吗"}, {"nhmn", "你好吗呢"}}) {
                c.clear();
                // Exercise append and deletion across the threshold, including
                // caches populated while the initial was still the final key.
                for (const char key : std::string(keys)) c.type(std::string(1, key));
                FCITX_ASSERT(c.candidateSet().contains(word) ==
                             (minimum > 0 && std::string(keys).size() >= minimum))
                    << shuangpin << " " << minimum << " " << keys;
            }
            c.erase(2, c.size());
            FCITX_ASSERT(c.candidateSet().contains("你好") ==
                         (minimum > 0 && minimum <= 2));
            c.clear();
            c.type(shuangpin ? "nihc" : "nihao");
            FCITX_ASSERT(c.candidateSet().contains("你好"));
            c.clear();
            c.type("n");
            FCITX_ASSERT(c.candidateSet().contains("你"));
            c.clear();
            c.type("nih");
            FCITX_ASSERT(c.candidateSet().contains("你好") ==
                         (minimum > 0 && minimum <= 3));
        }
        ime->setPartialSpLength(0);
        c.type("n'h");
        FCITX_ASSERT(!c.candidateSet().contains("你好"));
        c.clear();
        c.type(shuangpin ? "nihcnh" : "nihaonh");
        const auto first = std::ranges::find(c.candidates(), "你好", &SentenceResult::toString);
        FCITX_ASSERT(first != c.candidates().end());
        c.select(std::distance(c.candidates().begin(), first));
        FCITX_ASSERT(!c.candidateSet().contains("你好"));
        c.clear();
        c.type(shuangpin ? "nihcn" : "nihaon");
        const auto prefix = std::ranges::find(c.candidates(), "你好", &SentenceResult::toString);
        FCITX_ASSERT(prefix != c.candidates().end());
        c.select(std::distance(c.candidates().begin(), prefix));
        FCITX_ASSERT(c.candidateSet().contains("你"));
    }
}

void testUvMappingWithoutTypos() {
    auto ime = makeIME();
    ime->dict()->addWord(PinyinDictionary::SystemDict, "yu", "鱼", 0);
    ime->dict()->addWord(PinyinDictionary::SystemDict, "ju", "句", 0);
    for (const bool shuangpin : {false, true}) {
        PinyinContext c(ime.get());
        c.setUseShuangpin(shuangpin);
        for (const auto &[keys, word] : {std::pair{"yv", "鱼"}, {"jv", "句"}}) {
            ime->setFuzzyFlags(PinyinFuzzyFlag::None);
            c.type(keys);
            FCITX_ASSERT(!c.candidateSet().contains(word));
            ime->setFuzzyFlags(PinyinFuzzyFlag::V_U);
            c.type(keys);
            FCITX_ASSERT(c.candidateSet().contains(word)) << keys << " " << shuangpin;
            c.clear();
        }
    }
}
} // namespace

int main() {
    testPromotionAndLearning();
    testThresholdAndModes();
    testBounds();
    testCandidateLimitAndExplicitBoundaries();
    testAutomaticInitialFuzzy();
    testContextCorrectionRestriction();
    testAbbreviationThresholds();
    testUvMappingWithoutTypos();
}
