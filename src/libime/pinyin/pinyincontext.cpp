/*
 * SPDX-FileCopyrightText: 2017-2017 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "pinyincontext.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <boost/container_hash/hash.hpp>
#include <fcitx-utils/charutils.h>
#include <fcitx-utils/inputbuffer.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/macros.h>
#include <fcitx-utils/signals.h>
#include <fcitx-utils/stringutils.h>
#include <fcitx-utils/utf8.h>
#include "libime/core/historybigram.h"
#include "libime/core/inputbuffer.h"
#include "libime/core/languagemodel.h"
#include "libime/core/lattice.h"
#include "libime/core/segmentgraph.h"
#include "libime/core/userlanguagemodel.h"
#include "libime/pinyin/constants.h"
#include "pinyindecoder.h"
#include "pinyindecoder_p.h"
#include "pinyinencoder.h"
#include "pinyinime.h"
#include "pinyinmatchstate.h"
#include "shuangpinprofile.h"

namespace libime {

namespace {
enum class LearnWordResult {
    Normal,  /// word is consisted all from regular word from dict.
    Custom,  /// word is consisted with custom word (e.g. symbol replacement).
    Ignored, /// not learned as word.
};

enum class SelectedPinyinType {
    Normal,
    Custom,
    Separator,
};

// Get encoded full pinyin.
std::string sentenceEncodedFullPinyin(const SentenceResult &candidate) {
    std::string pinyin;
    size_t length = 0;
    for (const auto &node : candidate.sentence()) {
        if (!node->as<PinyinLatticeNode>().encodedPinyin().empty()) {
            length += node->as<PinyinLatticeNode>().encodedPinyin().size();
        }
    }
    pinyin.reserve(length);
    for (const auto &node : candidate.sentence()) {
        if (!node->as<PinyinLatticeNode>().encodedPinyin().empty()) {
            pinyin += node->as<PinyinLatticeNode>().encodedPinyin();
        }
    }
    return pinyin;
}

size_t candidateSelectTo(const SentenceResult &candidate) {
    return candidate.sentence().empty()
               ? 0
               : candidate.sentence().back()->to()->index();
}

struct SelectedPinyin {
    SelectedPinyin(size_t s, PinyinWordNode word, SelectedPinyinType type)
        : offset_(s), word_(std::move(word)), type_(type) {}

    const std::string &encodedPinyin() const { return word_.encodedPinyin(); }

    size_t offset_;
    PinyinWordNode word_;
    SelectedPinyinType type_;
};

struct SelectedSyllable {
    size_t from;
    size_t to;
    PinyinSyllable syllable;
    std::string input;
};

struct CandidateDedupByPinyin {
    std::string text_;
    std::string fullPinyin_;

    bool operator==(const CandidateDedupByPinyin &other) const {
        return text_ == other.text_ && fullPinyin_ == other.fullPinyin_;
    }
};

struct CandidateDedupByPinyinHash {
    size_t operator()(const CandidateDedupByPinyin &key) const {
        size_t seed = std::hash<std::string>()(key.text_);
        boost::hash_combine(seed, std::hash<std::string>()(key.fullPinyin_));
        return seed;
    }
};

CandidateDedupByPinyin candidateDedupByPinyin(const SentenceResult &candidate) {
    return {.text_ = candidate.toString(),
            .fullPinyin_ = sentenceEncodedFullPinyin(candidate)};
}

struct CandidateDedupBySelectRange {
    std::string text_;
    std::string end_;

    bool operator==(const CandidateDedupBySelectRange &other) const {
        return text_ == other.text_ && end_ == other.end_;
    }
};

struct CandidateDedupBySelectRangeHash {
    size_t operator()(const CandidateDedupBySelectRange &key) const {
        size_t seed = std::hash<std::string>()(key.text_);
        boost::hash_combine(seed, std::hash<std::string>()(key.end_));
        return seed;
    }
};

CandidateDedupBySelectRange
candidateDedupBySelectRange(const SentenceResult &candidate) {
    return {.text_ = candidate.toString(),
            .end_ = std::to_string(candidateSelectTo(candidate))};
}

bool hasCorrection(const SentenceResult &candidate) {
    return std::ranges::any_of(
        candidate.sentence(), [](const LatticeNode *node) {
            return node->as<PinyinLatticeNode>().isCorrection();
        });
}

} // namespace

class PinyinContextPrivate : public fcitx::QPtrHolder<PinyinContext> {
public:
    PinyinContextPrivate(PinyinContext *q, PinyinIME *ime)
        : QPtrHolder(q), ime_(ime), matchState_(q) {}

    std::vector<std::vector<SelectedPinyin>> selected_;
    std::vector<SelectedSyllable> selectedSyllables_;

    // Only populated in an isolated recovery decoder. Readings are already
    // constrained by the original context, including explicit confirmations.
    struct RecoverySyllables {
        size_t from;
        size_t to;
        MatchedPinyinSyllablesWithFuzzyFlags matches;
    };
    std::vector<RecoverySyllables> recoverySyllables_;

    bool sp_ = false;
    bool correctionEnabled_ = true;
    bool abbreviationEnabled_ = true;
    std::shared_ptr<const ShuangpinProfile> shuangpinProfile_;
    int maxSentenceLength_ = -1;
    PinyinIME *ime_;
    SegmentGraph segs_;
    Lattice lattice_;
    PinyinMatchState matchState_;
    std::vector<SentenceResult> candidates_;
    std::unordered_set<std::string> candidatesSet_;
    std::optional<CandidateDedupByPinyin> bestAbbreviation_;
    size_t bestAbbreviationEnd_ = 0;
    mutable bool candidatesToCursorNeedUpdate_ = true;
    mutable std::vector<SentenceResult> candidatesToCursor_;
    mutable std::unordered_set<std::string> candidatesToCursorSet_;
    std::vector<fcitx::ScopedConnection> conn_;
    std::list<PinyinWordNode> contextWords_;

    void resetMatching() {
        lattice_.clear();
        matchState_.clear();
        segs_ = SegmentGraph();
    }

    void discardSyllablesFrom(size_t pos) {
        const auto first = std::ranges::find_if(
            selectedSyllables_,
            [pos](const auto &syllable) { return syllable.to > pos; });
        if (first != selectedSyllables_.end()) {
            selectedSyllables_.erase(first, selectedSyllables_.end());
            resetMatching();
        }
    }

    size_t alignCursorToNextSegment() const {
        FCITX_Q();
        auto currentCursor = q->cursor();
        auto start = q->selectedLength();
        if (currentCursor < start) {
            return start;
        }
        while (segs_.nodes(currentCursor - start).empty() &&
               currentCursor < q->size()) {
            currentCursor += 1;
        }
        return currentCursor;
    }

    bool needCandidatesToCursor() const {
        FCITX_Q();
        if (q->cursor() == q->selectedLength()) {
            return false;
        }

        return alignCursorToNextSegment() != q->size();
    }

    void clearCandidates() {
        bestAbbreviation_.reset();
        bestAbbreviationEnd_ = 0;
        candidates_.clear();
        candidatesToCursor_.clear();
        candidatesToCursorNeedUpdate_ = false;
        candidatesSet_.clear();
        candidatesToCursorSet_.clear();
    }

    // Whether the whole input can be parsed as valid 2-key shuangpin
    // syllables. If so, partial shuangpin should not be promoted.
    bool hasFullShuangpinParse() const {
        std::vector<bool> reachable(segs_.size() + 1, false);
        reachable[0] = true;
        for (size_t i = 0; i < segs_.size(); i++) {
            if (!reachable[i]) {
                continue;
            }
            for (const auto &node : segs_.nodes(i)) {
                for (const auto &next : node.nexts()) {
                    if (next.index() - node.index() == 2) {
                        reachable[next.index()] = true;
                    }
                }
            }
        }
        return reachable[segs_.size()];
    }

    // A candidate that consumes the whole input with one key per segment,
    // i.e. the bare initial abbreviation.
    bool isPartialShuangpinCandidate(const SentenceResult &candidate) const {
        if (candidate.sentence().empty()) {
            return false;
        }
        const auto &back = candidate.sentence().back();
        if (candidate.sentence().front()->from()->index() != 0 ||
            back->to()->index() != segs_.size()) {
            return false;
        }
        for (const auto *node : candidate.sentence()) {
            const auto &path = node->path();
            // Long-word completion is not an initial abbreviation.
            if (node->as<PinyinLatticeNode>().encodedPinyin().size() !=
                (path.size() - 1) * 2) {
                return false;
            }
            for (size_t i = 1; i < path.size(); i++) {
                if (path[i]->index() - path[i - 1]->index() != 1) {
                    return false;
                }
            }
        }
        return true;
    }

    bool usesOrdinaryShuangpinRanking() const {
        FCITX_Q();
        const auto profile = q->shuangpinProfile();
        return q->useShuangpin() && profile && !profile->isGrouped();
    }

    const SentenceResult *compositionCandidate() const {
        FCITX_Q();
        if (usesOrdinaryShuangpinRanking()) {
            const auto remaining = q->size() - q->selectedLength();
            if (!remaining) {
                return nullptr;
            }
            // A prefix may win by score; composition boundaries must still
            // follow a complete path independently of the selection order.
            const auto full = std::ranges::find_if(
                candidates_, [remaining](const auto &candidate) {
                    return !candidate.sentence().empty() &&
                           candidate.sentence().front()->from()->index() == 0 &&
                           candidate.sentence().back()->to()->index() == remaining;
                });
            if (full != candidates_.end()) {
                return &*full;
            }
        }
        return candidates_.empty() ? nullptr : &candidates_.front();
    }

    bool isOrdinaryShuangpinAbbreviation(const SentenceResult &candidate) const {
        FCITX_Q();
        if (!usesOrdinaryShuangpinRanking() ||
            !q->fuzzyFlags().test(PinyinFuzzyFlag::PartialSp) ||
            ime_->partialSpLength() == 0 ||
            segs_.size() < ime_->partialSpLength() ||
            !selectedSyllables_.empty() || needCandidatesToCursor() ||
            segs_.data().find('\'') != std::string::npos ||
            hasCorrection(candidate) || !isPartialShuangpinCandidate(candidate)) {
            return false;
        }
        for (const auto *node : candidate.sentence()) {
            const auto &encoded = node->as<PinyinLatticeNode>().encodedPinyin();
            for (size_t i = 1; i < encoded.size(); i += 2) {
                if (PinyinEncoder::isFinalLetter(
                        static_cast<PinyinFinal>(encoded[i]))) {
                    return false;
                }
            }
        }
        return true;
    }

    // Retain the existing grouped-keyboard policy. Ordinary 26-key shuangpin
    // uses one score order and a frontend reservation instead of promotion.
    std::optional<CandidateDedupByPinyin> preferredGroupedShuangpin() const {
        FCITX_Q();
        if (candidates_.empty() || !q->useShuangpin() ||
            usesOrdinaryShuangpinRanking() || !selectedSyllables_.empty() ||
            needCandidatesToCursor() ||
            !q->fuzzyFlags().test(PinyinFuzzyFlag::PartialSp)) {
            return std::nullopt;
        }
        const std::string &keys = segs_.data();
        if (ime_->partialSpLength() == 0 ||
            keys.size() < ime_->partialSpLength() ||
            keys.find('\'') != std::string::npos || hasFullShuangpinParse()) {
            return std::nullopt;
        }
        const SentenceResult *bestPartial = nullptr;
        const SentenceResult *bestLearned = nullptr;
        for (const auto &candidate : candidates_) {
            if (hasCorrection(candidate)) {
                continue;
            }
            if (isPartialShuangpinCandidate(candidate) &&
                (!bestPartial || candidate.score() > bestPartial->score())) {
                bestPartial = &candidate;
            }
            // Normal committed history provides persistence and respects the
            // engine's learning switch. Two uses of the same two-Han word
            // override the abbreviation's initial preference.
            if (candidate.size() != 1 ||
                candidateSelectTo(candidate) != segs_.size()) {
                continue;
            }
            const auto *node = candidate.sentence().front();
            const auto &pinyin = node->as<PinyinLatticeNode>().encodedPinyin();
            const auto &word = node->word();
            if (node->from()->index() != 0 || pinyin.size() != 4 ||
                fcitx::utf8::length(word) != 2 ||
                ime_->model()->history().rawUnigramFrequency({word, pinyin}) <
                    2) {
                continue;
            }
            const auto characters = fcitx::utf8::MakeUTF8StringViewRange(word);
            const bool isHan = std::all_of(
                characters.begin(), characters.end(), [](auto character) {
                    const auto code = fcitx::utf8::getChar(character);
                    return (code >= 0x3400 && code <= 0x4DBF) ||
                           (code >= 0x4E00 && code <= 0x9FFF) ||
                           (code >= 0xF900 && code <= 0xFAFF) ||
                           (code >= 0x20000 && code <= 0x3FFFF);
                });
            if (isHan &&
                (!bestLearned || candidate.score() > bestLearned->score())) {
                bestLearned = &candidate;
            }
        }
        if (!bestPartial) {
            return std::nullopt;
        }
        return candidateDedupByPinyin(bestLearned ? *bestLearned
                                                  : *bestPartial);
    }

    void updateCandidatesToCursor() const {
        FCITX_Q();
        if (!candidatesToCursorNeedUpdate_) {
            return;
        }
        candidatesToCursorNeedUpdate_ = false;
        candidatesToCursor_.clear();
        candidatesToCursorSet_.clear();

        std::unordered_map<CandidateDedupByPinyin, size_t,
                           CandidateDedupByPinyinHash>
            duplicateByPinyin;
        auto insertCandidate = [this,
                                &duplicateByPinyin](SentenceResult candidate) {
            auto byPinyin = candidateDedupByPinyin(candidate);
            auto byPinyinIter = duplicateByPinyin.find(byPinyin);
            if (byPinyinIter != duplicateByPinyin.end()) {
                auto &oldCandidate = candidatesToCursor_[byPinyinIter->second];
                if (std::make_tuple(candidateSelectTo(candidate),
                                    candidate.score()) >
                    std::make_tuple(candidateSelectTo(oldCandidate),
                                    oldCandidate.score())) {
                    oldCandidate = std::move(candidate);
                }
                return;
            }

            candidatesToCursor_.push_back(std::move(candidate));
            duplicateByPinyin.emplace(byPinyin, candidatesToCursor_.size() - 1);
            candidatesToCursorSet_.insert(std::move(byPinyin.text_));
        };

        auto start = q->selectedLength();
        auto currentCursor = alignCursorToNextSegment();
        // Poke best sentence from lattice, ignore nbest option for now.
        auto nodeRange = lattice_.nodes(&segs_.node(currentCursor - start));
        if (!nodeRange.empty()) {
            insertCandidate(nodeRange.front().toSentenceResult());
        }
        for (const auto &candidate : candidates_) {
            const auto &sentence = candidate.sentence();
            if (sentence.size() == 1) {
                if (candidateSelectTo(candidate) + start > currentCursor) {
                    continue;
                }
                insertCandidate(candidate);
            } else if (sentence.size() > 1) {
                auto newSentence = sentence;
                while (!newSentence.empty() &&
                       candidateSelectTo(candidate) + start > currentCursor) {
                    newSentence.pop_back();
                }
                if (!newSentence.empty()) {
                    SentenceResult partial(newSentence,
                                           newSentence.back()->score());
                    insertCandidate(std::move(partial));
                }
            }
        }

        std::unordered_set<CandidateDedupBySelectRange,
                           CandidateDedupBySelectRangeHash>
            duplicateByRange;
        std::erase_if(
            candidatesToCursor_,
            [&duplicateByRange](const SentenceResult &candidate) {
                if (duplicateByRange
                        .insert(candidateDedupBySelectRange(candidate))
                        .second) {
                    return false;
                }
                return hasCorrection(candidate);
            });
    }

    template <typename FillSentence>
    void selectHelper(const FillSentence &fillSentence) {
        FCITX_Q();
        selected_.emplace_back();

        auto &selection = selected_.back();
        fillSentence(selection);
        // add some special code for handling separator at the end
        auto remain =
            std::string_view(q->userInput()).substr(q->selectedLength());
        if (!remain.empty()) {
            if (std::all_of(remain.begin(), remain.end(),
                            [](char c) { return c == '\''; })) {
                selection.emplace_back(q->size(), PinyinWordNode({}, 0),
                                       SelectedPinyinType::Separator);
            }
        }

        if (!selectedSyllables_.empty()) {
            // A selected word changes the offset of the remaining graph.
            resetMatching();
        }
        q->update();
    }

    void select(const SentenceResult &sentence) {
        FCITX_Q();
        auto offset = q->selectedLength();
        selectHelper([offset, &sentence,
                      this](std::vector<SelectedPinyin> &selection) {
            for (const auto &p : sentence.sentence()) {
                selection.emplace_back(
                    offset + p->to()->index(),
                    PinyinWordNode{
                        {p->word(), p->as<PinyinLatticeNode>().encodedPinyin()},
                        ime_->model()->index(p->word())},

                    SelectedPinyinType::Normal);
            }
        });
    }

    void selectCustom(size_t inputLength, std::string_view segment,
                      std::string_view encodedPinyin) {
        FCITX_Q();
        auto offset = q->selectedLength();
        selectHelper([this, offset, &segment, inputLength,
                      &encodedPinyin](std::vector<SelectedPinyin> &selection) {
            auto index = ime_->model()->index(segment);
            selection.emplace_back(
                offset + inputLength,
                PinyinWordNode{{segment, encodedPinyin}, index},
                SelectedPinyinType::Custom);
        });
    }

    std::tuple<LearnWordResult, std::string> learnWord() {
        std::string ss;
        std::string pinyin;
        if (selected_.empty()) {
            return {LearnWordResult::Ignored, ""};
        }
        // don't learn existing word.
        if (selected_.size() == 1 && selected_[0].size() == 1) {
            return {LearnWordResult::Ignored, ""};
        }
        // Validate the learning word.
        // All single || custom || length <= 4
        bool hasCustom = false;
        size_t totalPinyinLength = 0;
        bool isAllSingleWord = true;
        for (auto &s : selected_) {
            isAllSingleWord =
                isAllSingleWord &&
                (s.empty() || (s.size() == 1 &&
                               (s[0].type_ == SelectedPinyinType::Separator ||
                                s[0].encodedPinyin().size() == 2)));
            for (auto &item : s) {
                if (item.type_ == SelectedPinyinType::Separator) {
                    continue;
                }
                if (item.type_ == SelectedPinyinType::Custom) {
                    hasCustom = true;
                }
                // We can't learn non pinyin word.
                if (item.encodedPinyin().empty() ||
                    item.encodedPinyin().size() % 2 != 0) {
                    return {LearnWordResult::Ignored, ""};
                }
                totalPinyinLength += item.encodedPinyin().size() / 2;
            }
        }

        FCITX_Q();
        if (!hasCustom) {
            if ((!isAllSingleWord && totalPinyinLength > 4)) {
                return {LearnWordResult::Ignored, ""};
            }
            if (ime_->model()->containsNonUnigram(q->selectedWords())) {
                return {LearnWordResult::Ignored, ""};
            }
        }

        for (auto &s : selected_) {
            for (auto &item : s) {
                if (item.type_ == SelectedPinyinType::Separator) {
                    continue;
                }
                assert(!item.encodedPinyin().empty());
                assert(item.encodedPinyin().size() % 2 == 0);
                ss += item.word_.word();
                if (!pinyin.empty()) {
                    pinyin.push_back('\'');
                }
                pinyin += PinyinEncoder::decodeFullPinyin(item.encodedPinyin());
            }
        }

        if (auto opt = ime_->dict()->lookupWord(PinyinDictionary::UserDict,
                                                pinyin, ss)) {
            return {LearnWordResult::Ignored, ""};
        }

        ime_->dict()->addWord(PinyinDictionary::UserDict, pinyin, ss,
                              hasCustom ? -1 : 0);

        auto encodedPinyin = PinyinEncoder::encodeFullPinyinWithFlags(
            pinyin, PinyinFuzzyFlag::VE_UE);

        return {hasCustom ? LearnWordResult::Custom : LearnWordResult::Normal,
                std::string(encodedPinyin.data(), encodedPinyin.size())};
    }
};

void matchPinyinCase(std::string_view ref, std::string &actualPinyin) {
    if (ref.size() != fcitx::utf8::length(actualPinyin)) {
        return;
    }

    auto iter = fcitx::utf8::MakeUTF8CharIterator(actualPinyin.begin(),
                                                  actualPinyin.end());
    for (size_t i = 0; i < ref.size(); ++i, ++iter) {
        if (fcitx::charutils::isupper(ref[i])) {
            auto charRange = iter.charRange();
            if (iter.charLength() == 1 &&
                fcitx::charutils::islower(iter.view()[0])) {
                *charRange.first = fcitx::charutils::toupper(*charRange.first);
            } else if (*iter == 0x00fc) {
                *charRange.first = 0xc3;
                *std::next(charRange.first) = 0x9c;
            }
        }
    }
}

PinyinContext::PinyinContext(PinyinIME *ime)
    : InputBuffer(fcitx::InputBufferOption::AsciiOnly),
      d_ptr(std::make_unique<PinyinContextPrivate>(this, ime)) {
    FCITX_D();
    d->conn_.emplace_back(
        ime->connect<PinyinIME::optionChanged>([this]() { clear(); }));
    d->conn_.emplace_back(
        ime->dict()->connect<PinyinDictionary::dictionaryChanged>(
            [this](size_t) {
                FCITX_D();
                d->matchState_.clear();
            }));
}

PinyinContext::~PinyinContext() {}

void PinyinContext::setUseShuangpin(bool sp) {
    FCITX_D();
    if (d->sp_ != sp && !d->selectedSyllables_.empty()) {
        d->selectedSyllables_.clear();
        d->resetMatching();
    }
    d->sp_ = sp;
    d->matchState_.clear();
}

bool PinyinContext::useShuangpin() const {
    FCITX_D();
    return d->sp_;
}

PinyinFuzzyFlags PinyinContext::fuzzyFlags() const {
    FCITX_D();
    auto flags = d->ime_->fuzzyFlags();
    return d->correctionEnabled_ ? flags
                                 : flags.unset(PinyinFuzzyFlag::Correction);
}

void PinyinContext::setCorrectionEnabled(bool enabled) {
    FCITX_D();
    if (d->correctionEnabled_ == enabled) {
        return;
    }
    d->correctionEnabled_ = enabled;
    d->resetMatching();
    update();
}

void PinyinContext::setShuangpinProfile(
    std::shared_ptr<const ShuangpinProfile> profile) {
    FCITX_D();
    if (d->shuangpinProfile_ == profile) {
        return;
    }
    clear();
    d->shuangpinProfile_ = std::move(profile);
}

std::shared_ptr<const ShuangpinProfile>
PinyinContext::shuangpinProfile() const {
    FCITX_D();
    return d->shuangpinProfile_ ? d->shuangpinProfile_
                                : d->ime_->shuangpinProfile();
}

size_t PinyinContext::nextSyllableOffset() const {
    FCITX_D();
    auto offset = selectedLength();
    if (!d->selectedSyllables_.empty()) {
        offset = std::max(offset, d->selectedSyllables_.back().to);
    }
    while (offset < size() && userInput()[offset] == '\'') {
        offset++;
    }
    return offset;
}

bool PinyinContext::selectSyllable(size_t from, size_t to,
                                   PinyinSyllable syllable) {
    FCITX_D();
    if (!useShuangpin() || from != nextSyllableOffset() || from >= to ||
        to > size() || to - from > 2) {
        return false;
    }
    const auto input = std::string_view(userInput()).substr(from, to - from);
    if (input.find('\'') != std::string_view::npos) {
        return false;
    }
    const auto matches = PinyinEncoder::shuangpinToSyllablesWithFuzzyFlags(
        input, *shuangpinProfile(), fuzzyFlags());
    const auto valid =
        std::ranges::any_of(matches, [&syllable](const auto &initial) {
            return initial.first == syllable.initial() &&
                   std::ranges::any_of(
                       initial.second, [&syllable](const auto &final) {
                           return final.first == syllable.final();
                       });
        });
    if (!valid) {
        return false;
    }
    d->selectedSyllables_.push_back({from, to, syllable, userInput()});
    d->resetMatching();
    update();
    return true;
}

bool PinyinContext::cancelSyllable() {
    FCITX_D();
    if (d->selectedSyllables_.empty() || cursor() != size()) {
        return false;
    }
    const auto &last = d->selectedSyllables_.back();
    if (last.from < selectedLength() || last.input != userInput()) {
        return false;
    }
    d->selectedSyllables_.pop_back();
    d->resetMatching();
    update();
    return true;
}

std::vector<ShuangpinRecoveryCandidate>
PinyinContext::shuangpinRecoveryCandidates(size_t limit) const {
    FCITX_D();
    const auto profile = shuangpinProfile();
    const auto flags = fuzzyFlags();
    if (!limit || !useShuangpin() || !profile || profile->isGrouped() ||
        cursor() != size() || !flags.test(PinyinFuzzyFlag::Correction)) {
        return {};
    }
    const auto start = selectedLength();
    const auto keys = std::string_view(userInput()).substr(start);
    if (keys.size() < 4 || std::ranges::any_of(keys, [](char c) {
            return !fcitx::charutils::islower(c) && c != ';' && c != '\'';
        })) {
        return {};
    }

    PinyinContext recovery(d->ime_);
    auto *temporary = recovery.d_func();
    temporary->sp_ = true;
    temporary->shuangpinProfile_ = profile;
    // Bypass PinyinContext::typeImpl: this buffer is only the helper for the
    // local lattice below and must not start an ordinary, unconstrained decode.
    recovery.InputBuffer::typeImpl(keys.data(), keys.size());
    SegmentGraph graph{std::string(keys)};
    size_t invalidPairs = 0;
    size_t syllables = 0;
    for (size_t from = 0; from < keys.size();) {
        if (keys[from] == '\'') {
            auto to = from + 1;
            while (to < keys.size() && keys[to] == '\'') {
                ++to;
            }
            graph.addNext(from, to);
            from = to;
            continue;
        }
        if (from + 1 >= keys.size() || keys[from + 1] == '\'' ||
            ++syllables > 35) {
            return {};
        }
        const auto to = from + 2;
        const auto pair = keys.substr(from, 2);
        auto matches = PinyinEncoder::shuangpinToSyllablesWithFuzzyFlags(
            pair, *profile, flags.unset(PinyinFuzzyFlag::Correction));
        const auto retainComplete = [](auto &matches) {
            std::erase_if(matches, [](auto &initial) {
                std::erase_if(initial.second, [](const auto &final) {
                    return final.first == PinyinFinal::Invalid;
                });
                return initial.first == PinyinInitial::Invalid ||
                       initial.second.empty();
            });
        };
        retainComplete(matches);
        if (matches.empty()) {
            if (++invalidPairs > 2) {
                return {};
            }
            matches = PinyinEncoder::shuangpinToSyllablesWithFuzzyFlags(
                pair, *profile, flags);
            retainComplete(matches);
        }
        if (matches.empty() ||
            !constrainSyllables(start + from, start + to, matches) ||
            matches.empty()) {
            return {};
        }
        temporary->recoverySyllables_.push_back(
            {from, to, std::move(matches)});
        graph.addNext(from, to);
        from = to;
    }
    if (!invalidPairs) {
        return {};
    }

    // No recovery cache is shared across input, cursor, scheme, confirmation,
    // or language-model states. Position-dependent dictionary caches are
    // disabled by hasSyllableConstraints(), just like manual confirmations.
    Lattice lattice;
    const auto count = std::min<size_t>(limit, 3);
    d->ime_->decoder()->decode(lattice, graph, count, state(),
                               d->ime_->maxDistance(), d->ime_->minPath(),
                               8, 32, &temporary->matchState_);
    std::vector<SentenceResult> pool;
    for (size_t i = 0; i < lattice.sentenceSize(); ++i) {
        pool.push_back(lattice.sentence(i));
    }
    // The decoder's N-best deduplicates text. Also inspect complete lattice
    // paths so different readings of the same text retain their own identity.
    const auto endNodes = lattice.nodes(nullptr);
    if (endNodes.empty()) {
        return {};
    }
    const auto &eos = endNodes.front();
    for (const auto &node : lattice.nodes(&graph.end())) {
        State afterEOS;
        const auto endScore = d->ime_->model()->score(node.state(), eos, afterEOS);
        pool.push_back(node.toSentenceResult(endScore));
    }
    std::stable_sort(pool.begin(), pool.end(), std::greater<>());
    std::vector<ShuangpinRecoveryCandidate> results;
    for (const auto &candidate : pool) {
        if (candidateSelectTo(candidate) != keys.size()) {
            continue;
        }
        auto encoded = sentenceEncodedFullPinyin(candidate);
        auto word = candidate.toString();
        if (word.empty() || encoded.size() != syllables * 2 ||
            std::ranges::any_of(results, [&](const auto &old) {
                return old.word == word && old.encodedPinyin == encoded;
            })) {
            continue;
        }
        results.push_back({std::move(word), std::move(encoded), keys.size()});
        if (results.size() == count) {
            break;
        }
    }
    return results;
}

bool PinyinContext::hasSyllableConstraints() const {
    FCITX_D();
    return !d->selectedSyllables_.empty() || !d->recoverySyllables_.empty() ||
           !d->abbreviationEnabled_;
}

bool PinyinContext::constrainSyllables(
    size_t from, size_t to,
    MatchedPinyinSyllablesWithFuzzyFlags &syllables) const {
    FCITX_D();
    if (!d->recoverySyllables_.empty()) {
        const auto match = std::ranges::find_if(
            d->recoverySyllables_, [from, to](const auto &entry) {
                return entry.from == from && entry.to == to;
            });
        if (match == d->recoverySyllables_.end()) {
            return false;
        }
        syllables = match->matches;
        return true;
    }
    // A single remaining key always offers candidates, as do an unfinished
    // final syllable and explicitly confirmed boundaries.
    // Other bare initials require the abbreviation threshold.
    const bool allowIncompleteSyllable =
        to == size() ||
        std::ranges::any_of(d->selectedSyllables_, [from, to](const auto &s) {
            return s.from == from && s.to == to;
        });
    if (!d->abbreviationEnabled_ && !allowIncompleteSyllable &&
        size() - selectedLength() > 1) {
        std::erase_if(syllables, [](auto &initial) {
            std::erase_if(initial.second, [](const auto &final) {
                return final.first == PinyinFinal::Invalid;
            });
            return initial.second.empty();
        });
    }
    for (const auto &selected : d->selectedSyllables_) {
        if (from >= selected.to || to <= selected.from) {
            continue;
        }
        // A confirmation fixes both its reading and its segment boundaries.
        if (from != selected.from || to != selected.to) {
            return false;
        }
        std::erase_if(syllables, [&selected](auto &initial) {
            if (initial.first != selected.syllable.initial()) {
                return true;
            }
            std::erase_if(initial.second, [&selected](const auto &final) {
                return selected.syllable.final() != PinyinFinal::Invalid &&
                       final.first != selected.syllable.final();
            });
            return initial.second.empty();
        });
    }
    return true;
}

void PinyinContext::setMaxSentenceLength(int length) {
    FCITX_D();
    d->maxSentenceLength_ = length;
    d->matchState_.clear();
}

int PinyinContext::maxSentenceLength() const {
    FCITX_D();
    return d->maxSentenceLength_;
}

bool PinyinContext::typeImpl(const char *s, size_t length) {
    FCITX_D();
    const auto *composition = d->compositionCandidate();
    if (d->maxSentenceLength_ > 0 &&
        (composition || d->usesOrdinaryShuangpinRanking())) {
        auto size = 0;
        size_t covered = 0;
        if (composition) {
            for (const auto &s : composition->sentence()) {
                // This is pinyin length + 1
                auto segLength = s->path().size();
                size +=
                    std::max(static_cast<decltype(segLength)>(1), segLength) - 1;
                covered = s->to()->index();
            }
        }
        const auto remaining = this->size() - selectedLength();
        if (d->usesOrdinaryShuangpinRanking() && remaining > covered) {
            size += remaining - covered;
        }
        if (size > d->maxSentenceLength_) {
            return false;
        }
    }
    auto changed = cancelTill(cursor());
    const auto insertion = cursor();
    const auto typed = InputBuffer::typeImpl(s, length);
    if (typed) {
        d->discardSyllablesFrom(insertion);
    }
    changed = typed || changed;
    if (changed) {
        update();
    }
    return changed;
}

void PinyinContext::erase(size_t from, size_t to) {
    if (from == to) {
        return;
    }

    FCITX_D();
    d->discardSyllablesFrom(from);
    // check if erase everything
    if (from == 0 && to >= size()) {
        d->clearCandidates();
        d->selected_.clear();
        d->selectedSyllables_.clear();
        d->lattice_.clear();
        d->matchState_.clear();
        d->segs_ = SegmentGraph();
    } else {
        cancelTill(from);
    }
    InputBuffer::erase(from, to);

    if (!empty()) {
        update();
    }
}

void PinyinContext::setCursor(size_t pos) {
    FCITX_D();
    auto oldCursor = cursor();
    auto cancelled = cancelTill(pos);
    InputBuffer::setCursor(pos);
    if (cancelled) {
        update();
    } else {
        if (cursor() != oldCursor) {
            d->candidatesToCursorNeedUpdate_ = true;
        }
    }
}

int PinyinContext::pinyinBeforeCursor() const {
    FCITX_D();
    auto len = selectedLength();
    auto c = cursor();
    if (c < len) {
        return -1;
    }
    c -= len;
    const auto *composition = d->compositionCandidate();
    size_t covered = 0;
    if (composition) {
        for (const auto &s : composition->sentence()) {
            covered = s->to()->index();
            for (auto iter = s->path().begin(),
                      end = std::prev(s->path().end());
                 iter < end; iter++) {
                auto from = (*iter)->index();
                auto to = (*std::next(iter))->index();
                if (to >= c) {
                    return from + len;
                }
            }
        }
    }
    if (d->usesOrdinaryShuangpinRanking() && c > covered &&
        c <= size() - len) {
        return len + c - 1;
    }
    return -1;
}

int PinyinContext::pinyinAfterCursor() const {
    FCITX_D();
    auto len = selectedLength();
    auto c = cursor();
    if (c < len) {
        return -1;
    }
    c -= len;
    const auto *composition = d->compositionCandidate();
    size_t covered = 0;
    if (composition) {
        for (const auto &s : composition->sentence()) {
            covered = s->to()->index();
            for (auto iter = s->path().begin(),
                      end = std::prev(s->path().end());
                 iter < end; iter++) {
                auto to = (*std::next(iter))->index();
                if (to > c) {
                    return to + len;
                }
            }
        }
    }
    if (d->usesOrdinaryShuangpinRanking() && c >= covered &&
        c < size() - len) {
        return len + c + 1;
    }
    return -1;
}

const std::vector<SentenceResult> &PinyinContext::candidates() const {
    FCITX_D();
    return d->candidates_;
}

bool PinyinContext::isShuangpinAbbreviation(const SentenceResult &candidate) const {
    FCITX_D();
    return d->isOrdinaryShuangpinAbbreviation(candidate) ||
           isBestShuangpinAbbreviation(candidate);
}

bool PinyinContext::isBestShuangpinAbbreviation(
    const SentenceResult &candidate) const {
    FCITX_D();
    return d->bestAbbreviation_ &&
           candidateSelectTo(candidate) == d->bestAbbreviationEnd_ &&
           d->usesOrdinaryShuangpinRanking() && !d->needCandidatesToCursor() &&
           candidateDedupByPinyin(candidate) == *d->bestAbbreviation_;
}

const std::unordered_set<std::string> &PinyinContext::candidateSet() const {
    FCITX_D();
    return d->candidatesSet_;
}

const std::vector<SentenceResult> &PinyinContext::candidatesToCursor() const {
    FCITX_D();
    if (!d->needCandidatesToCursor()) {
        return d->candidates_;
    }
    d->updateCandidatesToCursor();
    return d->candidatesToCursor_;
}

const std::unordered_set<std::string> &
PinyinContext::candidatesToCursorSet() const {
    FCITX_D();
    if (!d->needCandidatesToCursor()) {
        return d->candidatesSet_;
    }
    d->updateCandidatesToCursor();
    return d->candidatesToCursorSet_;
}

void PinyinContext::select(size_t idx) {
    FCITX_D();
    const auto &candidates = this->candidates();
    assert(idx < candidates.size());
    d->select(candidates[idx]);
}

void PinyinContext::selectCandidatesToCursor(size_t idx) {
    FCITX_D();
    const auto &candidates = this->candidatesToCursor();
    assert(idx < candidates.size());
    d->select(candidates[idx]);
}

void PinyinContext::selectCustom(size_t inputLength, std::string_view segment,
                                 std::string_view encodedPinyin) {
    FCITX_D();
    if (inputLength == 0 && selectedLength() + inputLength > size()) {
        throw std::out_of_range("Invalid input length");
    }
    if (encodedPinyin.size() % 2 != 0) {
        throw std::invalid_argument("Invalid encoded pinyin");
    }
    d->selectCustom(inputLength, segment, encodedPinyin);
}

bool PinyinContext::cancelTill(size_t pos) {
    bool cancelled = false;
    while (selectedLength() > pos) {
        cancel();
        cancelled = true;
    }
    return cancelled;
}

void PinyinContext::cancel() {
    FCITX_D();
    if (!d->selected_.empty()) {
        d->selected_.pop_back();

        // There is no point to reuse any existing matching state.
        // For example, cancel from (tao zi) tao zi to  tao zi tao zi.
        // Even if they share the same prefix, the start state won't be same.
        d->lattice_.clear();
        d->matchState_.clear();
        d->segs_ = SegmentGraph();
    }
    update();
}

State PinyinContext::state() const {
    FCITX_D();
    auto *model = d->ime_->model();
    State state = model->nullState();
    for (const auto &word : d->contextWords_) {
        State temp;
        model->score(state, word, temp);
        state = std::move(temp);
    }
    for (const auto &s : d->selected_) {
        for (const auto &item : s) {
            if (item.word_.word().empty()) {
                continue;
            }
            State temp;
            model->score(state, item.word_, temp);
            state = std::move(temp);
        }
    }
    return state;
}

void PinyinContext::update() {
    FCITX_D();
    if (empty()) {
        clear();
        return;
    }

    if (selected()) {
        d->clearCandidates();
    } else {
        size_t start = 0;
        State state = this->state();
        if (!d->selected_.empty()) {
            start = d->selected_.back().back().offset_;
        }
        const auto minimum = d->ime_->partialSpLength();
        const bool abbreviationEnabled =
            minimum > 0 && size() - start >= minimum &&
            (!useShuangpin() || fuzzyFlags().test(PinyinFuzzyFlag::PartialSp));
        // Appending turns the previous unfinished final into an interior
        // segment. Its cached wildcard matches must not survive this change
        // or crossing the abbreviation threshold in either direction.
        if (!abbreviationEnabled || !d->abbreviationEnabled_) {
            d->resetMatching();
        }
        d->abbreviationEnabled_ = abbreviationEnabled;
        SegmentGraph newGraph;
        if (auto spProfile = d->matchState_.shuangpinProfile()) {
            if (d->selectedSyllables_.empty()) {
                newGraph = PinyinEncoder::parseUserShuangpin(
                    userInput().substr(start), *spProfile, fuzzyFlags(),
                    d->ime_->partialSpLength());
            } else {
                newGraph = SegmentGraph(userInput().substr(start));
                auto appendUnconfirmed = [&](size_t from, size_t to) {
                    if (from == to) {
                        return;
                    }
                    auto part = PinyinEncoder::parseUserShuangpin(
                        userInput().substr(from, to - from), *spProfile,
                        fuzzyFlags(), d->ime_->partialSpLength());
                    for (size_t i = 0; i < part.size(); i++) {
                        for (const auto &node : part.nodes(i)) {
                            for (const auto &next : node.nexts()) {
                                newGraph.addNext(from - start + node.index(),
                                                 from - start + next.index());
                            }
                        }
                    }
                };
                auto offset = start;
                for (const auto &selected : d->selectedSyllables_) {
                    if (selected.from < start) {
                        continue;
                    }
                    appendUnconfirmed(offset, selected.from);
                    // A confirmed partial syllable remains a separate segment
                    // when more keys are appended, like an apostrophe
                    // separator.
                    newGraph.addNext(selected.from - start,
                                     selected.to - start);
                    offset = selected.to;
                }
                appendUnconfirmed(offset, size());
            }
        } else {
            newGraph = PinyinEncoder::parseUserPinyin(
                userInput().substr(start),
                d->matchState_.correctionProfile().get(), fuzzyFlags());
        }
        d->segs_.merge(
            newGraph,
            [d](const std::unordered_set<const SegmentGraphNode *> &nodes) {
                d->lattice_.discardNode(nodes);
                d->matchState_.discardNode(nodes);
            });
        auto &graph = d->segs_;

        d->ime_->decoder()->decode(d->lattice_, d->segs_, d->ime_->nbest(),
                                   state, d->ime_->maxDistance(),
                                   d->ime_->minPath(), d->ime_->beamSize(),
                                   d->ime_->frameSize(), &d->matchState_);

        d->clearCandidates();

        // Add n-best result.
        for (size_t i = 0, e = d->lattice_.sentenceSize(); i < e; i++) {
            d->candidates_.push_back(d->lattice_.sentence(i));
        }

        const auto *bos = &graph.start();

        auto beginSize = d->candidates_.size();
        for (size_t i = graph.size(); i > 0; i--) {
            float min = 0;
            float max = -std::numeric_limits<float>::max();
            auto distancePenalty = d->ime_->model()->unknownPenalty() /
                                   PINYIN_DISTANCE_PENALTY_FACTOR;

            // Enumerate over all the lattice node, if from == bos, this is
            // a dictionary word match.
            // Add all words that does not contain pinyin correction.
            for (const auto &graphNode : graph.nodes(i)) {
                auto distance = graph.distanceToEnd(graphNode);
                auto adjust = static_cast<float>(distance) * distancePenalty;
                for (const auto &latticeNode : d->lattice_.nodes(&graphNode)) {
                    if (latticeNode.from() == bos &&
                        !static_cast<const PinyinLatticeNode &>(latticeNode)
                             .isCorrection()) {
                        if (!d->ime_->model()->isNodeUnknown(latticeNode)) {
                            min = std::min(latticeNode.score(), min);
                            max = std::max(latticeNode.score(), max);
                        }
                        d->candidates_.push_back(
                            latticeNode.toSentenceResult(adjust));
                    }
                }
            }

            // Filter correction word based on score
            for (const auto &graphNode : graph.nodes(i)) {
                auto distance = graph.distanceToEnd(graphNode);
                auto adjust = static_cast<float>(distance) * distancePenalty;
                for (const auto &latticeNode : d->lattice_.nodes(&graphNode)) {
                    if (latticeNode.from() == bos &&
                        static_cast<const PinyinLatticeNode &>(latticeNode)
                            .isCorrection()) {
                        if ((latticeNode.score() > min &&
                             latticeNode.score() + d->ime_->maxDistance() >
                                 max) ||
                            static_cast<const PinyinLatticeNode &>(latticeNode)
                                    .encodedPinyin()
                                    .size() <= 2) {
                            d->candidates_.push_back(
                                latticeNode.toSentenceResult(adjust));
                        }
                    }
                }
            }

            // This part is the phrase that's constructable from lattice.
            for (const auto &graphNode : graph.nodes(i)) {
                auto distance = graph.distanceToEnd(graphNode);
                auto adjust = static_cast<float>(distance) * distancePenalty;
                for (const auto &latticeNode : d->lattice_.nodes(&graphNode)) {
                    if (latticeNode.from() != bos &&
                        latticeNode.score() > min &&
                        latticeNode.score() + d->ime_->maxDistance() > max &&
                        !static_cast<const PinyinLatticeNode &>(latticeNode)
                             .anyCorrectionOnPath()) {
                        d->candidates_.push_back(
                            latticeNode.toSentenceResult(adjust));
                    }
                }
            }
        }
        const bool ordinaryShuangpin = d->usesOrdinaryShuangpinRanking();
        if (ordinaryShuangpin) {
            // N-best includes EOS, while appended dictionary paths used to
            // omit it. Replay each represented path once from the same model
            // state: this also handles N-best's alternative predecessor paths.
            const auto endNodes = d->lattice_.nodes(nullptr);
            if (!endNodes.empty()) {
                const auto &eos = endNodes.front();
                const auto distancePenalty = d->ime_->model()->unknownPenalty() /
                                             PINYIN_DISTANCE_PENALTY_FACTOR;
                for (auto &candidate : d->candidates_) {
                    State current = state;
                    float score = 0;
                    for (const auto *node : candidate.sentence()) {
                        State next;
                        score += d->ime_->model()->score(current, *node, next) +
                                 node->cost();
                        current = next;
                    }
                    State afterEOS;
                    score += d->ime_->model()->score(current, eos, afterEOS);
                    if (!candidate.sentence().empty()) {
                        const auto distance = graph.distanceToEnd(
                            *candidate.sentence().back()->to());
                        score += static_cast<float>(distance) * distancePenalty;
                    }
                    candidate.setScore(score);
                }
            }
            std::stable_sort(d->candidates_.begin(), d->candidates_.end(),
                             std::greater<>());
            for (const auto &candidate : d->candidates_) {
                if (d->isOrdinaryShuangpinAbbreviation(candidate)) {
                    d->bestAbbreviation_ = candidateDedupByPinyin(candidate);
                    d->bestAbbreviationEnd_ = candidateSelectTo(candidate);
                    break;
                }
            }
        } else {
            std::sort(d->candidates_.begin() + beginSize, d->candidates_.end(),
                      std::greater<>());
        }
        const auto preferred = ordinaryShuangpin
                                   ? d->bestAbbreviation_
                                   : d->preferredGroupedShuangpin();
        if (ordinaryShuangpin) {
            // A single ranked stream has no positional N-best exemption. The
            // reserved abbreviation remains inside the configured word budget.
            beginSize = 0;
        }
        {
            size_t index = 0;
            size_t count = 0;
            const auto limit = d->ime_->wordCandidateLimit();
            // Reserve one ordinary word slot for the preferred candidate,
            // while retaining the original n-best exemptions.
            bool reservePreferred =
                limit && preferred &&
                std::none_of(
                    d->candidates_.begin(), d->candidates_.begin() + beginSize,
                    [&preferred](const auto &candidate) {
                        return candidateDedupByPinyin(candidate) == *preferred;
                    });
            std::unordered_set<CandidateDedupByPinyin,
                               CandidateDedupByPinyinHash>
                duplicateByPinyin;
            std::unordered_set<CandidateDedupBySelectRange,
                               CandidateDedupBySelectRangeHash>
                duplicateByRange;
            auto &candidatesSet = d->candidatesSet_;
            candidatesSet.clear();
            std::erase_if(
                d->candidates_,
                [&candidatesSet, &duplicateByPinyin, &duplicateByRange, &index,
                 &count, &reservePreferred, &preferred, beginSize,
                 limit](const SentenceResult &candidate) {
                    bool beforeBeginSize = index++ < beginSize;
                    auto byPinyin = candidateDedupByPinyin(candidate);
                    if (duplicateByPinyin.contains(byPinyin)) {
                        return true;
                    }
                    auto byRange = candidateDedupBySelectRange(candidate);
                    if (duplicateByRange.contains(byRange) &&
                        hasCorrection(candidate)) {
                        return true;
                    }

                    if (!beforeBeginSize && limit) {
                        const bool isSinglePinyinWord =
                            candidate.sentence().size() == 1 &&
                            candidate.sentence()
                                    .front()
                                    ->as<PinyinLatticeNode>()
                                    .encodedPinyin()
                                    .size() == 2;
                        if (!isSinglePinyinWord) {
                            const bool isPreferred =
                                preferred && byPinyin == *preferred;
                            if (!isPreferred &&
                                count >= limit - size_t{reservePreferred}) {
                                return true;
                            }
                            if (isPreferred) {
                                reservePreferred = false;
                            }
                            count++;
                        }
                    }

                    candidatesSet.insert(byPinyin.text_);
                    duplicateByPinyin.insert(std::move(byPinyin));
                    duplicateByRange.insert(std::move(byRange));
                    return false;
                });
        }

        if (preferred && !ordinaryShuangpin) {
            auto it = std::ranges::find_if(
                d->candidates_, [&preferred](const auto &candidate) {
                    return candidateDedupByPinyin(candidate) == *preferred;
                });
            if (it != d->candidates_.end()) {
                std::rotate(d->candidates_.begin(), it, std::next(it));
            }
        }

        d->candidatesToCursorNeedUpdate_ = true;
    }

    if (cursor() < selectedLength()) {
        setCursor(selectedLength());
    }
}

bool PinyinContext::selected() const {
    FCITX_D();
    if (userInput().empty()) {
        return false;
    }

    if (!d->selected_.empty()) {
        if (d->selected_.back().back().offset_ == size()) {
            return true;
        }
    }

    return false;
}

std::string PinyinContext::selectedSentence() const {
    FCITX_D();
    std::string ss;
    for (const auto &s : d->selected_) {
        for (const auto &item : s) {
            ss += item.word_.word();
        }
    }
    return ss;
}

size_t PinyinContext::selectedLength() const {
    FCITX_D();
    if (!d->selected_.empty()) {
        return d->selected_.back().back().offset_;
    }
    return 0;
}

std::string PinyinContext::preedit() const {
    return preedit(ime()->preeditMode());
}

std::pair<std::string, size_t> PinyinContext::preeditWithCursor() const {
    return preeditWithCursor(ime()->preeditMode());
}

std::string PinyinContext::preedit(PinyinPreeditMode mode) const {
    return preeditWithCursor(mode).first;
}

std::pair<std::string, size_t>
PinyinContext::preeditWithCursor(PinyinPreeditMode mode) const {
    FCITX_D();
    std::string ss = selectedSentence();
    const auto len = selectedLength();
    auto c = cursor();
    size_t actualCursor = ss.size();
    // should not happen
    c = std::max(c, len);

    auto resultSize = ss.size();

    const auto *preeditCandidate = d->compositionCandidate();
    const bool ordinaryShuangpin = d->usesOrdinaryShuangpinRanking();
    bool first = true;
    size_t covered = 0;
    if (preeditCandidate) {
        for (const auto &node : preeditCandidate->sentence()) {
            for (auto iter = node->path().begin(),
                      end = std::prev(node->path().end());
                 iter < end; iter++) {
                if (!first) {
                    ss += " ";
                    resultSize += 1;
                } else {
                    first = false;
                }
                auto from = (*iter)->index();
                auto to = (*std::next(iter))->index();
                covered = to;
                size_t cursorInPinyin = c - from - len;
                const size_t startPivot = resultSize;
                auto pinyin = d->segs_.segment(from, to);
                MatchedPinyinSyllables syls;
                if (mode == PinyinPreeditMode::Pinyin) {
                    // The reason that we don't use fuzzy flag from option is
                    // that we'd like to keep the preedit as is. Otherwise,
                    // "qign" would be displayed as "qing", which would be
                    // confusing to user about what is actually being typed.
                    syls = useShuangpin() ? PinyinEncoder::shuangpinToSyllables(
                                                pinyin, *shuangpinProfile(),
                                                PinyinFuzzyFlag::None)
                                          : PinyinEncoder::stringToSyllables(
                                                pinyin, PinyinFuzzyFlag::None);
                }
                std::string actualPinyin;
                if (!syls.empty() && !syls.front().second.empty()) {
                    std::string_view candidatePinyin =
                        node->as<PinyinLatticeNode>().encodedPinyin();
                    auto nthPinyin = std::distance(node->path().begin(), iter);
                    PinyinInitial bestInitial = syls[0].first;
                    PinyinFinal bestFinal = syls[0].second[0].first;

                    // Try to match the candidate syllables from all possible
                    // none-fuzzy possible syls.
                    if (static_cast<size_t>((nthPinyin * 2) + 2) <=
                        candidatePinyin.size()) {
                        auto candidateInitial = static_cast<PinyinInitial>(
                            candidatePinyin[nthPinyin * 2]);
                        auto candidateFinal = static_cast<PinyinFinal>(
                            candidatePinyin[(nthPinyin * 2) + 1]);

                        bool found = false;
                        for (const auto &initial : syls) {
                            for (const auto &[final, fuzzy] : initial.second) {
                                if (fuzzy) {
                                    continue;
                                }
                                if (candidateInitial == initial.first &&
                                    (final == PinyinFinal::Invalid ||
                                     candidateFinal == final)) {
                                    bestInitial = initial.first;
                                    if (final != PinyinFinal::Invalid) {
                                        bestFinal = final;
                                    }
                                    found = true;
                                    break;
                                }
                            }
                            if (found) {
                                break;
                            }
                        }
                    }

                    actualPinyin = PinyinEncoder::initialFinalToPinyinString(
                        bestInitial, bestFinal);
                    if (!useShuangpin()) {
                        matchPinyinCase(pinyin, actualPinyin);
                    }
                }
                if (!actualPinyin.empty()) {
                    if (c > from + len && c <= to + len) {
                        if (useShuangpin()) {
                            switch (cursorInPinyin) {
                            case 0:
                                break;
                            case 1:
                                if (pinyin.size() == 2 &&
                                    syls[0].first == PinyinInitial::Zero) {
                                    actualPinyin = fcitx::stringutils::concat(
                                        "_", actualPinyin);
                                }
                                // Zero case, we just append one.
                                if (syls[0].first != PinyinInitial::Zero) {
                                    cursorInPinyin =
                                        PinyinEncoder::initialToString(
                                            syls[0].first)
                                            .size();
                                }
                                break;
                            default:
                                cursorInPinyin = actualPinyin.size();
                                break;
                            }
                        } else {
                            cursorInPinyin =
                                std::min(actualPinyin.size(), cursorInPinyin);
                            cursorInPinyin = fcitx::utf8::ncharByteLength(
                                actualPinyin.begin(), cursorInPinyin);
                        }
                    }
                    ss.append(actualPinyin);
                    resultSize += actualPinyin.size();
                } else {
                    ss.append(pinyin.data(), pinyin.size());
                    resultSize += pinyin.size();
                }
                if (c > from + len && c <= to + len) {
                    actualCursor = startPivot + cursorInPinyin;
                }
            }
        }
    }
    const auto remaining = size() - len;
    if (ordinaryShuangpin && covered < remaining) {
        // A complete path can be absent after candidate limiting, or when the
        // remaining input has no dictionary match. Keep that raw suffix and
        // its cursor mapping even though it cannot contribute a candidate.
        if (!first) {
            ss += " ";
            ++resultSize;
        }
        if (c >= len + covered) {
            actualCursor = resultSize + c - len - covered;
        }
        const auto suffix = std::string_view(userInput()).substr(len + covered);
        ss.append(suffix.data(), suffix.size());
        resultSize += suffix.size();
    }
    if (c == size()) {
        actualCursor = resultSize;
    }
    return {ss, actualCursor};
}

std::vector<std::string> PinyinContext::selectedWords() const {
    FCITX_D();
    std::vector<std::string> newSentence;
    for (const auto &s : d->selected_) {
        for (const auto &item : s) {
            if (item.type_ != SelectedPinyinType::Separator) {
                newSentence.push_back(item.word_.word());
            }
        }
    }
    return newSentence;
}

std::vector<HistoryBigram::WordWithCode>
PinyinContext::selectedWordsWithPinyin() const {
    FCITX_D();
    std::vector<HistoryBigram::WordWithCode> newSentence;
    for (const auto &s : d->selected_) {
        for (const auto &item : s) {
            if (item.type_ != SelectedPinyinType::Separator) {
                newSentence.emplace_back(item.word_.word(),
                                         item.encodedPinyin());
            }
        }
    }
    return newSentence;
}

std::string PinyinContext::selectedFullPinyin() const {
    FCITX_D();
    std::string pinyin;
    for (const auto &s : d->selected_) {
        for (const auto &item : s) {
            if (!item.encodedPinyin().empty()) {
                if (!pinyin.empty()) {
                    pinyin.push_back('\'');
                }
                pinyin += PinyinEncoder::decodeFullPinyin(item.encodedPinyin());
            }
        }
    }
    return pinyin;
}

std::string PinyinContext::candidateFullPinyin(size_t idx) const {
    FCITX_D();
    return candidateFullPinyin(d->candidates_[idx]);
}

std::string
PinyinContext::candidateFullPinyin(const SentenceResult &candidate) const {
    std::string pinyin;
    for (const auto &node : candidate.sentence()) {
        if (!node->as<PinyinLatticeNode>().encodedPinyin().empty()) {
            if (!pinyin.empty()) {
                pinyin.push_back('\'');
            }
            pinyin += PinyinEncoder::decodeFullPinyin(
                node->as<PinyinLatticeNode>().encodedPinyin());
        }
    }
    return pinyin;
}

void PinyinContext::learn() {
    FCITX_D();
    if (!selected()) {
        return;
    }

    std::vector<HistoryBigram::WordWithCode> newSentence;
    if (auto [result, encodedWordPinyin] = d->learnWord();
        result != LearnWordResult::Ignored) {
        // Do not insert custom to history for the first time.
        if (result == LearnWordResult::Normal) {
            // Create new sentence with the whole new learned word.
            newSentence.push_back({sentence(), encodedWordPinyin});
        } else {
            return;
        }
    } else {
        newSentence = selectedWordsWithPinyin();
    }

    if (std::ranges::any_of(newSentence, [](const auto &word) {
            return word.second.empty();
        })) {
        // Don't add to history if there is any non-pinyin word.
        return;
    }

    auto context = contextWordsWithPinyin();
    d->ime_->model()->history().addWithContext(contextWordsWithPinyin(),
                                               std::move(newSentence));
}

void PinyinContext::setContextWords(
    const std::vector<std::string> &contextWords) {
    FCITX_D();
    d->contextWords_.clear();
    appendContextWords(contextWords);
}

void PinyinContext::clearContextWords() {
    FCITX_D();
    d->contextWords_.clear();
}

void PinyinContext::appendContextWords(
    const std::vector<std::string> &contextWords) {
    FCITX_D();

    size_t needed = LanguageModel::maxOrder() - 1;

    for (const auto &word :
         std::span{contextWords}.last(std::min(contextWords.size(), needed))) {
        d->contextWords_.push_back(
            PinyinWordNode({word, ""}, d->ime_->model()->index(word)));
    }
    while (d->contextWords_.size() > needed) {
        d->contextWords_.pop_front();
    }
}

std::vector<std::string> PinyinContext::contextWords() const {
    FCITX_D();
    std::vector<std::string> words;
    words.reserve(d->contextWords_.size());
    for (const auto &word : d->contextWords_) {
        words.push_back(word.word());
    }
    return words;
}

void PinyinContext::setContextWordsWithPinyin(
    const std::vector<HistoryBigram::WordWithCode> &contextWordsWithPinyin) {
    FCITX_D();
    d->contextWords_.clear();
    appendContextWordsWithPinyin(contextWordsWithPinyin);
}

void PinyinContext::appendContextWordsWithPinyin(
    const std::vector<HistoryBigram::WordWithCode> &contextWordsWithPinyin) {
    FCITX_D();

    size_t needed = LanguageModel::maxOrder() - 1;

    for (const auto &word : std::span{contextWordsWithPinyin}.last(
             std::min(contextWordsWithPinyin.size(), needed))) {
        d->contextWords_.push_back(
            PinyinWordNode(word, d->ime_->model()->index(word.first)));
    }
    while (d->contextWords_.size() > needed) {
        d->contextWords_.pop_front();
    }
}

std::vector<HistoryBigram::WordWithCode>
PinyinContext::contextWordsWithPinyin() const {
    FCITX_D();
    std::vector<HistoryBigram::WordWithCode> words;
    words.reserve(d->contextWords_.size());
    for (const auto &word : d->contextWords_) {
        words.push_back({word.word(), word.encodedPinyin()});
    }
    return words;
}

bool PinyinContext::learnWord() { return false; }

PinyinIME *PinyinContext::ime() const {
    FCITX_D();
    return d->ime_;
}
} // namespace libime
