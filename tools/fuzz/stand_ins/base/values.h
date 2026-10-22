// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's base/values.h, used only by
// tools/fuzz/molt_fuzz.py (the GN fuzzer_test targets use the real //base).
// The subset of the API MoltBrowser's code uses, with the same contracts
// where they matter to a caller: the Get*() accessors abort on the wrong
// type (Chromium CHECKs), GetDouble() also reads an int, FindDouble()
// finds ints, Set() on an existing key replaces it, and a DictValue keeps
// its keys sorted (Chromium's is a flat_map). As in this tree, there is no
// nested Value::Dict / Value::List: use DictValue and ListValue.

#ifndef MOLT_FUZZ_STAND_INS_BASE_VALUES_H_
#define MOLT_FUZZ_STAND_INS_BASE_VALUES_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "base/check.h"

namespace base {

class DictValue;
class ListValue;

class Value {
 public:
  enum class Type {
    NONE = 0,
    BOOLEAN,
    INTEGER,
    DOUBLE,
    STRING,
    BINARY,
    DICT,
    LIST,
  };
  using BlobStorage = std::vector<uint8_t>;

  Value() = default;
  explicit Value(Type type);
  explicit Value(bool b) : data_(b) {}
  explicit Value(int i) : data_(i) {}
  explicit Value(double d) : data_(d) {}
  explicit Value(std::string_view s) : data_(std::string(s)) {}
  explicit Value(const char* s) : data_(std::string(s)) {}
  explicit Value(std::string&& s) : data_(std::move(s)) {}
  explicit Value(const std::string& s) : data_(s) {}
  explicit Value(BlobStorage blob) : data_(std::move(blob)) {}
  explicit Value(DictValue&& d);
  explicit Value(ListValue&& l);
  // Pointers would otherwise convert to bool.
  explicit Value(const void*) = delete;
  Value(Value&&) noexcept = default;
  Value& operator=(Value&&) noexcept = default;
  Value(const Value&) = delete;
  Value& operator=(const Value&) = delete;
  ~Value();

  Type type() const { return static_cast<Type>(data_.index()); }
  bool is_none() const { return type() == Type::NONE; }
  bool is_bool() const { return type() == Type::BOOLEAN; }
  bool is_int() const { return type() == Type::INTEGER; }
  bool is_double() const { return type() == Type::DOUBLE; }
  bool is_string() const { return type() == Type::STRING; }
  bool is_blob() const { return type() == Type::BINARY; }
  bool is_dict() const { return type() == Type::DICT; }
  bool is_list() const { return type() == Type::LIST; }

  bool GetBool() const {
    CHECK(is_bool());
    return std::get<bool>(data_);
  }
  int GetInt() const {
    CHECK(is_int());
    return std::get<int>(data_);
  }
  double GetDouble() const {
    if (is_int()) return std::get<int>(data_);
    CHECK(is_double());
    return std::get<double>(data_);
  }
  const std::string& GetString() const {
    CHECK(is_string());
    return std::get<std::string>(data_);
  }
  std::string& GetString() {
    CHECK(is_string());
    return std::get<std::string>(data_);
  }
  const BlobStorage& GetBlob() const {
    CHECK(is_blob());
    return std::get<BlobStorage>(data_);
  }
  const DictValue& GetDict() const {
    CHECK(is_dict());
    return *std::get<std::unique_ptr<DictValue>>(data_);
  }
  DictValue& GetDict() {
    CHECK(is_dict());
    return *std::get<std::unique_ptr<DictValue>>(data_);
  }
  const ListValue& GetList() const {
    CHECK(is_list());
    return *std::get<std::unique_ptr<ListValue>>(data_);
  }
  ListValue& GetList() {
    CHECK(is_list());
    return *std::get<std::unique_ptr<ListValue>>(data_);
  }

  std::optional<bool> GetIfBool() const {
    return is_bool() ? std::optional<bool>(GetBool()) : std::nullopt;
  }
  std::optional<int> GetIfInt() const {
    return is_int() ? std::optional<int>(GetInt()) : std::nullopt;
  }
  std::optional<double> GetIfDouble() const {
    return is_int() || is_double() ? std::optional<double>(GetDouble())
                                   : std::nullopt;
  }
  const std::string* GetIfString() const {
    return is_string() ? &GetString() : nullptr;
  }
  const DictValue* GetIfDict() const { return is_dict() ? &GetDict() : nullptr; }
  DictValue* GetIfDict() { return is_dict() ? &GetDict() : nullptr; }
  const ListValue* GetIfList() const { return is_list() ? &GetList() : nullptr; }
  ListValue* GetIfList() { return is_list() ? &GetList() : nullptr; }

  Value Clone() const;
  friend bool operator==(const Value& a, const Value& b);

 private:
  std::variant<std::monostate,
               bool,
               int,
               double,
               std::string,
               BlobStorage,
               std::unique_ptr<DictValue>,
               std::unique_ptr<ListValue>>
      data_;
};

class DictValue {
 public:
  using Items = std::vector<std::pair<std::string, Value>>;

  DictValue() = default;
  DictValue(DictValue&&) noexcept = default;
  DictValue& operator=(DictValue&&) noexcept = default;
  DictValue(const DictValue&) = delete;
  DictValue& operator=(const DictValue&) = delete;

  bool empty() const { return items_.empty(); }
  size_t size() const { return items_.size(); }
  void clear() { items_.clear(); }
  DictValue Clone() const {
    DictValue d;
    for (const auto& kv : items_) {
      d.items_.emplace_back(kv.first, kv.second.Clone());
    }
    return d;
  }
  bool contains(std::string_view key) const { return Find(key) != nullptr; }
  const Value* Find(std::string_view key) const {
    auto it = LowerBound(key);
    return it != items_.end() && it->first == key ? &it->second : nullptr;
  }
  Value* Find(std::string_view key) {
    auto it = LowerBound(key);
    return it != items_.end() && it->first == key ? &it->second : nullptr;
  }
  std::optional<bool> FindBool(std::string_view key) const {
    const Value* v = Find(key);
    return v ? v->GetIfBool() : std::nullopt;
  }
  std::optional<int> FindInt(std::string_view key) const {
    const Value* v = Find(key);
    return v ? v->GetIfInt() : std::nullopt;
  }
  std::optional<double> FindDouble(std::string_view key) const {
    const Value* v = Find(key);
    return v ? v->GetIfDouble() : std::nullopt;
  }
  const std::string* FindString(std::string_view key) const {
    const Value* v = Find(key);
    return v ? v->GetIfString() : nullptr;
  }
  std::string* FindString(std::string_view key) {
    Value* v = Find(key);
    return v && v->is_string() ? &v->GetString() : nullptr;
  }
  const DictValue* FindDict(std::string_view key) const {
    const Value* v = Find(key);
    return v ? v->GetIfDict() : nullptr;
  }
  DictValue* FindDict(std::string_view key) {
    Value* v = Find(key);
    return v ? v->GetIfDict() : nullptr;
  }
  const ListValue* FindList(std::string_view key) const {
    const Value* v = Find(key);
    return v ? v->GetIfList() : nullptr;
  }
  ListValue* FindList(std::string_view key) {
    Value* v = Find(key);
    return v ? v->GetIfList() : nullptr;
  }
  bool Remove(std::string_view key) {
    auto it = LowerBound(key);
    if (it == items_.end() || it->first != key) return false;
    items_.erase(it);
    return true;
  }
  std::optional<Value> Extract(std::string_view key) {
    auto it = LowerBound(key);
    if (it == items_.end() || it->first != key) return std::nullopt;
    Value v = std::move(it->second);
    items_.erase(it);
    return v;
  }

  // Keys are kept sorted, as Chromium's flat_map keeps them: iteration (and
  // so JSONWriter's output) is in key order.
  Value* Set(std::string_view key, Value&& value) & {
    auto it = LowerBound(key);
    if (it != items_.end() && it->first == key) {
      it->second = std::move(value);
      return &it->second;
    }
    it = items_.emplace(it, std::string(key), std::move(value));
    return &it->second;
  }
  Value* Set(std::string_view key, bool v) & { return Set(key, Value(v)); }
  Value* Set(std::string_view key, int v) & { return Set(key, Value(v)); }
  Value* Set(std::string_view key, double v) & { return Set(key, Value(v)); }
  Value* Set(std::string_view key, std::string_view v) & {
    return Set(key, Value(v));
  }
  Value* Set(std::string_view key, const char* v) & {
    return Set(key, Value(v));
  }
  Value* Set(std::string_view key, const std::string& v) & {
    return Set(key, Value(v));
  }
  Value* Set(std::string_view key, std::string&& v) & {
    return Set(key, Value(std::move(v)));
  }
  Value* Set(std::string_view key, DictValue&& v) & {
    return Set(key, Value(std::move(v)));
  }
  Value* Set(std::string_view key, ListValue&& v) & {
    return Set(key, Value(std::move(v)));
  }
  Value* Set(std::string_view key, const void*) & = delete;
  template <typename T>
  DictValue&& Set(std::string_view key, T&& value) && {
    Set(key, std::forward<T>(value));
    return std::move(*this);
  }

  // Like Chromium's, iterators yield std::pair<const std::string&, Value&>
  // by value (so `for (const auto kv : dict)` copies references, not
  // values).
  template <bool kConst>
  class IteratorT {
   public:
    using Inner =
        std::conditional_t<kConst, Items::const_iterator, Items::iterator>;
    using reference =
        std::pair<const std::string&,
                  std::conditional_t<kConst, const Value&, Value&>>;
    using value_type = reference;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;
    struct pointer {
      reference ref;
      const reference* operator->() const { return &ref; }
    };

    IteratorT() = default;
    explicit IteratorT(Inner it) : it_(it) {}
    reference operator*() const { return reference(it_->first, it_->second); }
    pointer operator->() const { return pointer{**this}; }
    IteratorT& operator++() {
      ++it_;
      return *this;
    }
    IteratorT operator++(int) {
      IteratorT before = *this;
      ++it_;
      return before;
    }
    friend bool operator==(const IteratorT& a, const IteratorT& b) {
      return a.it_ == b.it_;
    }

   private:
    Inner it_{};
  };
  using iterator = IteratorT<false>;
  using const_iterator = IteratorT<true>;

  const_iterator begin() const { return const_iterator(items_.begin()); }
  const_iterator end() const { return const_iterator(items_.end()); }
  iterator begin() { return iterator(items_.begin()); }
  iterator end() { return iterator(items_.end()); }

  friend bool operator==(const DictValue& a, const DictValue& b);

 private:
  Items::iterator LowerBound(std::string_view key) {
    return std::lower_bound(
        items_.begin(), items_.end(), key,
        [](const auto& item, std::string_view k) { return item.first < k; });
  }
  Items::const_iterator LowerBound(std::string_view key) const {
    return std::lower_bound(
        items_.begin(), items_.end(), key,
        [](const auto& item, std::string_view k) { return item.first < k; });
  }

  Items items_;
};

class ListValue {
 public:
  using Items = std::vector<Value>;

  ListValue() = default;
  ListValue(ListValue&&) noexcept = default;
  ListValue& operator=(ListValue&&) noexcept = default;
  ListValue(const ListValue&) = delete;
  ListValue& operator=(const ListValue&) = delete;

  bool empty() const { return items_.empty(); }
  size_t size() const { return items_.size(); }
  void clear() { items_.clear(); }
  void reserve(size_t n) { items_.reserve(n); }
  const Value& operator[](size_t i) const {
    CHECK(i < items_.size());
    return items_[i];
  }
  Value& operator[](size_t i) {
    CHECK(i < items_.size());
    return items_[i];
  }
  const Value& front() const {
    CHECK(!items_.empty());
    return items_.front();
  }
  const Value& back() const {
    CHECK(!items_.empty());
    return items_.back();
  }
  ListValue Clone() const {
    ListValue l;
    for (const auto& v : items_) l.items_.push_back(v.Clone());
    return l;
  }
  void Append(Value&& v) & { items_.push_back(std::move(v)); }
  void Append(bool v) & { items_.emplace_back(v); }
  void Append(int v) & { items_.emplace_back(v); }
  void Append(double v) & { items_.emplace_back(v); }
  void Append(std::string_view v) & { items_.emplace_back(v); }
  void Append(const char* v) & { items_.emplace_back(v); }
  void Append(const std::string& v) & { items_.emplace_back(v); }
  void Append(std::string&& v) & { items_.emplace_back(std::move(v)); }
  void Append(DictValue&& v) & { items_.emplace_back(std::move(v)); }
  void Append(ListValue&& v) & { items_.emplace_back(std::move(v)); }
  void Append(const void*) & = delete;
  template <typename T>
  ListValue&& Append(T&& value) && {
    Append(std::forward<T>(value));
    return std::move(*this);
  }
  Items::iterator erase(Items::iterator pos) { return items_.erase(pos); }

  Items::const_iterator begin() const { return items_.begin(); }
  Items::const_iterator end() const { return items_.end(); }
  Items::iterator begin() { return items_.begin(); }
  Items::iterator end() { return items_.end(); }

  friend bool operator==(const ListValue& a, const ListValue& b);

 private:
  Items items_;
};

inline Value::Value(Type type) {
  switch (type) {
    case Type::NONE:
      break;
    case Type::BOOLEAN:
      data_ = false;
      break;
    case Type::INTEGER:
      data_ = 0;
      break;
    case Type::DOUBLE:
      data_ = 0.0;
      break;
    case Type::STRING:
      data_ = std::string();
      break;
    case Type::BINARY:
      data_ = BlobStorage();
      break;
    case Type::DICT:
      data_ = std::make_unique<DictValue>();
      break;
    case Type::LIST:
      data_ = std::make_unique<ListValue>();
      break;
  }
}
inline Value::Value(DictValue&& d)
    : data_(std::make_unique<DictValue>(std::move(d))) {}
inline Value::Value(ListValue&& l)
    : data_(std::make_unique<ListValue>(std::move(l))) {}
inline Value::~Value() = default;

inline Value Value::Clone() const {
  switch (type()) {
    case Type::NONE:
      return Value();
    case Type::BOOLEAN:
      return Value(GetBool());
    case Type::INTEGER:
      return Value(GetInt());
    case Type::DOUBLE:
      return Value(std::get<double>(data_));
    case Type::STRING:
      return Value(GetString());
    case Type::BINARY:
      return Value(GetBlob());
    case Type::DICT:
      return Value(GetDict().Clone());
    case Type::LIST:
      return Value(GetList().Clone());
  }
  return Value();
}

inline bool operator==(const Value& a, const Value& b) {
  if (a.type() != b.type()) return false;
  switch (a.type()) {
    case Value::Type::NONE:
      return true;
    case Value::Type::BOOLEAN:
      return a.GetBool() == b.GetBool();
    case Value::Type::INTEGER:
      return a.GetInt() == b.GetInt();
    case Value::Type::DOUBLE:
      return a.GetDouble() == b.GetDouble();
    case Value::Type::STRING:
      return a.GetString() == b.GetString();
    case Value::Type::BINARY:
      return a.GetBlob() == b.GetBlob();
    case Value::Type::DICT:
      return a.GetDict() == b.GetDict();
    case Value::Type::LIST:
      return a.GetList() == b.GetList();
  }
  return false;
}

inline bool operator==(const DictValue& a, const DictValue& b) {
  if (a.size() != b.size()) return false;
  auto it = b.begin();
  for (const auto& [key, value] : a) {
    if (it->first != key || !(value == it->second)) return false;
    ++it;
  }
  return true;
}

inline bool operator==(const ListValue& a, const ListValue& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!(a[i] == b[i])) return false;
  }
  return true;
}

}  // namespace base

#endif  // MOLT_FUZZ_STAND_INS_BASE_VALUES_H_
