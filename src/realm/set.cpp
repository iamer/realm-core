/*************************************************************************
 *
 * Copyright 2020 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include "realm/set.hpp"

#include "realm/array_basic.hpp"
#include "realm/array_binary.hpp"
#include "realm/array_bool.hpp"
#include "realm/array_decimal128.hpp"
#include "realm/array_fixed_bytes.hpp"
#include "realm/array_integer.hpp"
#include "realm/array_mixed.hpp"
#include "realm/array_string.hpp"
#include "realm/array_timestamp.hpp"
#include "realm/array_typed_link.hpp"
#include "realm/replication.hpp"

#include <numeric> // std::iota

namespace realm {

template <typename T>
UpdateStatus Set<T>::update_if_needed() const
{
    auto status = Base::update_if_needed();
    switch (status) {
        case UpdateStatus::Detached: {
            m_tree.reset();
            return UpdateStatus::Detached;
        }
        case UpdateStatus::NoChange:
            if (m_tree && m_tree->is_attached()) {
                return UpdateStatus::NoChange;
            }
            // The tree has not been initialized yet for this accessor, so
            // perform lazy initialization by treating it as an update.
            [[fallthrough]];
        case UpdateStatus::Updated: {
            bool attached = init_from_parent(false);
            return attached ? UpdateStatus::Updated : UpdateStatus::Detached;
        }
    }
    REALM_UNREACHABLE();
}

template <typename T>
UpdateStatus Set<T>::ensure_created()
{
    auto status = Base::ensure_created();
    switch (status) {
        case UpdateStatus::Detached:
            break; // Not possible (would have thrown earlier).
        case UpdateStatus::NoChange: {
            if (m_tree && m_tree->is_attached()) {
                return UpdateStatus::NoChange;
            }
            // The tree has not been initialized yet for this accessor, so
            // perform lazy initialization by treating it as an update.
            [[fallthrough]];
        }
        case UpdateStatus::Updated: {
            bool attached = init_from_parent(true);
            REALM_ASSERT(attached);
            return attached ? UpdateStatus::Updated : UpdateStatus::Detached;
        }
    }

    REALM_UNREACHABLE();
}

static bool do_init_from_parent(BPlusTreeBase& tree, bool allow_create)
{
    if (tree.init_from_parent()) {
        // All is well
        return true;
    }

    if (!allow_create) {
        return false;
    }

    // The ref in the column was NULL, create the tree in place.
    tree.create();
    REALM_ASSERT(tree.is_attached());
    return true;
}

template <typename T>
bool Set<T>::init_from_parent(bool allow_create) const
{
    if (!m_tree) {
        m_tree.reset(new BPlusTree<T>(m_obj.get_alloc()));
        const ArrayParent* parent = this;
        m_tree->set_parent(const_cast<ArrayParent*>(parent), 0);
    }
    return do_init_from_parent(*m_tree, allow_create);
}

SetBasePtr Obj::get_setbase_ptr(ColKey col_key) const
{
    auto attr = get_table()->get_column_attr(col_key);
    REALM_ASSERT(attr.test(col_attr_Set));
    bool nullable = attr.test(col_attr_Nullable);

    switch (get_table()->get_column_type(col_key)) {
        case type_Int:
            if (nullable)
                return std::make_unique<Set<util::Optional<Int>>>(*this, col_key);
            return std::make_unique<Set<Int>>(*this, col_key);
        case type_Bool:
            if (nullable)
                return std::make_unique<Set<util::Optional<Bool>>>(*this, col_key);
            return std::make_unique<Set<Bool>>(*this, col_key);
        case type_Float:
            if (nullable)
                return std::make_unique<Set<util::Optional<Float>>>(*this, col_key);
            return std::make_unique<Set<Float>>(*this, col_key);
        case type_Double:
            if (nullable)
                return std::make_unique<Set<util::Optional<Double>>>(*this, col_key);
            return std::make_unique<Set<Double>>(*this, col_key);
        case type_String:
            return std::make_unique<Set<String>>(*this, col_key);
        case type_Binary:
            return std::make_unique<Set<Binary>>(*this, col_key);
        case type_Timestamp:
            return std::make_unique<Set<Timestamp>>(*this, col_key);
        case type_Decimal:
            return std::make_unique<Set<Decimal128>>(*this, col_key);
        case type_ObjectId:
            if (nullable)
                return std::make_unique<Set<util::Optional<ObjectId>>>(*this, col_key);
            return std::make_unique<Set<ObjectId>>(*this, col_key);
        case type_UUID:
            if (nullable)
                return std::make_unique<Set<util::Optional<UUID>>>(*this, col_key);
            return std::make_unique<Set<UUID>>(*this, col_key);
        case type_TypedLink:
            return std::make_unique<Set<ObjLink>>(*this, col_key);
        case type_Mixed:
            return std::make_unique<Set<Mixed>>(*this, col_key);
        case type_Link:
            return std::make_unique<LnkSet>(*this, col_key);
        default:
            REALM_TERMINATE("Unsupported column type.");
    }
}

void SetBase::insert_repl(Replication* repl, size_t index, Mixed value) const
{
    repl->set_insert(*this, index, value);
}

void SetBase::erase_repl(Replication* repl, size_t index, Mixed value) const
{
    repl->set_erase(*this, index, value);
}

void SetBase::clear_repl(Replication* repl) const
{
    repl->set_clear(*this);
}

namespace {
template <typename T>
std::vector<T> convert_to_set(const CollectionBase& rhs)
{
    std::vector<T> vec;
    vec.reserve(rhs.size());
    for (Mixed value : rhs) {
        vec.push_back(value.get<T>());
    }
    std::sort(vec.begin(), vec.end(), SetElementLessThan<T>());
    vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
    return vec;
}

template <>
std::vector<Mixed> convert_to_set<Mixed>(const CollectionBase& rhs)
{
    std::vector<Mixed> mixed(rhs.begin(), rhs.end());
    std::sort(mixed.begin(), mixed.end(), SetElementLessThan<Mixed>());
    mixed.erase(std::unique(mixed.begin(), mixed.end()), mixed.end());
    return mixed;
}

template <typename Set, typename T, typename Fn>
auto to_set_if_needed(const SetBase& set, const CollectionBase& rhs, Fn fn)
{
    SetElementLessThan<T> cmp;
    auto& typed_set = static_cast<const Set&>(set);
    if (typeid(rhs) == typeid(Set)) {
        return fn(typed_set, static_cast<const Set&>(rhs), cmp);
    }
    return fn(typed_set, convert_to_set<T>(rhs), cmp);
}

template <typename Fn>
auto as_typed_sets(const SetBase& set, const CollectionBase& rhs, Fn fn)
{
    // Set<StringData> and Set<BinaryData> compare using `char` rather than
    // `unsigned char`, resulting in a different order from Set<Mixed> on
    // platforms where `char` is signed. This means that the elements will not
    // be ordered properly when using SetElementLessThan<Mixed>, and we need
    // to use the typed comparisons.
    // This can go away in the next major version, as we've fixed the order
    // of non-Mixed sets.
    switch (set.get_col_key().get_type()) {
        case col_type_String:
            return to_set_if_needed<Set<StringData>, StringData>(set, rhs, fn);
        case col_type_Binary:
            return to_set_if_needed<Set<BinaryData>, BinaryData>(set, rhs, fn);
        default:
            return to_set_if_needed<SetBase, Mixed>(set, rhs, fn);
    }
}
} // anonymous namespace

bool SetBase::is_subset_of(const CollectionBase& rhs) const
{
    return as_typed_sets(*this, rhs, [](auto&& lhs, auto&& rhs, auto cmp) {
        return std::includes(rhs.begin(), rhs.end(), lhs.begin(), lhs.end(), cmp);
    });
}

bool SetBase::is_strict_subset_of(const CollectionBase& rhs) const
{
    return size() != rhs.size() && is_subset_of(rhs);
}

bool SetBase::is_superset_of(const CollectionBase& rhs) const
{
    return as_typed_sets(*this, rhs, [](auto&& lhs, auto&& rhs, auto cmp) {
        return std::includes(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), cmp);
    });
}

bool SetBase::is_strict_superset_of(const CollectionBase& rhs) const
{
    return as_typed_sets(*this, rhs, [](auto&& lhs, auto&& rhs, auto cmp) {
        return lhs.size() > rhs.size() && std::includes(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), cmp);
    });
}

bool SetBase::intersects(const CollectionBase& rhs) const
{
    return as_typed_sets(*this, rhs, [](auto&& lhs, auto&& rhs, auto cmp) {
        auto first = rhs.begin(), last = rhs.end();
        auto it = lhs.begin();
        while (it != lhs.end() && first != last) {
            if (cmp(*it, *first)) {
                ++it;
            }
            else if (cmp(*first, *it)) {
                ++first;
            }
            else {
                return true;
            }
        }
        return false;
    });
}

bool SetBase::set_equals(const CollectionBase& rhs) const
{
    return as_typed_sets(*this, rhs, [](auto&& lhs, auto&& rhs, auto cmp) {
        return lhs.size() == rhs.size() && std::includes(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), cmp);
    });
}

void SetBase::assign_union(const CollectionBase& rhs)
{
    if (*this == rhs) {
        // Union of a set with itself is itself
        return;
    }
    return as_typed_sets(*this, rhs, [&](auto&& lhs, auto&& rhs, auto cmp) {
        std::vector<Mixed> the_diff;
        std::set_difference(rhs.begin(), rhs.end(), lhs.begin(), lhs.end(), std::back_inserter(the_diff), cmp);
        // 'the_diff' now contains all the elements that are in foreign set, but not in 'this'
        // Now insert those elements
        for (auto&& value : the_diff) {
            insert_any(value);
        }
    });
}

void SetBase::assign_intersection(const CollectionBase& rhs)
{
    if (*this == rhs) {
        // Intersection of a set with itself is itself
        return;
    }
    return as_typed_sets(*this, rhs, [&](auto&& lhs, auto&& rhs, auto cmp) {
        std::vector<Mixed> intersection;
        std::set_intersection(rhs.begin(), rhs.end(), lhs.begin(), lhs.end(), std::back_inserter(intersection), cmp);
        clear();
        // Elements in intersection comes from foreign set, so ok to use here
        for (auto&& value : intersection) {
            insert_any(value);
        }
    });
}

void SetBase::assign_difference(const CollectionBase& rhs)
{
    if (*this == rhs) {
        // Difference between a set and itself is empty
        clear();
        return;
    }
    return as_typed_sets(*this, rhs, [&](auto&& lhs, auto&& rhs, auto cmp) {
        std::vector<Mixed> intersection;
        std::set_intersection(rhs.begin(), rhs.end(), lhs.begin(), lhs.end(), std::back_inserter(intersection), cmp);
        // 'intersection' now contains all the elements that are in both foreign set and 'this'.
        // Remove those elements. The elements comes from the foreign set, so ok to refer to.
        for (auto&& value : intersection) {
            erase_any(value);
        }
    });
}

void SetBase::assign_symmetric_difference(const CollectionBase& rhs)
{
    if (*this == rhs) {
        // Difference between a set and itself is empty
        clear();
        return;
    }
    return as_typed_sets(*this, rhs, [&](auto&& lhs, auto&& rhs, auto cmp) {
        std::vector<Mixed> difference;
        std::set_difference(rhs.begin(), rhs.end(), lhs.begin(), lhs.end(), std::back_inserter(difference), cmp);
        std::vector<Mixed> intersection;
        std::set_intersection(rhs.begin(), rhs.end(), lhs.begin(), lhs.end(), std::back_inserter(intersection), cmp);
        // Now remove the common elements and add the differences
        for (auto&& value : intersection) {
            erase_any(value);
        }
        for (auto&& value : difference) {
            insert_any(value);
        }
    });
}

template <>
void Set<ObjKey>::do_insert(size_t ndx, ObjKey target_key)
{
    auto origin_table = m_obj.get_table();
    auto target_table_key = origin_table->get_opposite_table_key(m_col_key);
    m_obj.set_backlink(m_col_key, {target_table_key, target_key});
    tree().insert(ndx, target_key);
    if (target_key.is_unresolved()) {
        m_tree->set_context_flag(true);
    }
}

template <>
void Set<ObjKey>::do_erase(size_t ndx)
{
    auto origin_table = m_obj.get_table();
    auto target_table_key = origin_table->get_opposite_table_key(m_col_key);
    ObjKey old_key = get(ndx);
    CascadeState state(old_key.is_unresolved() ? CascadeState::Mode::All : CascadeState::Mode::Strong);

    bool recurse = m_obj.remove_backlink(m_col_key, {target_table_key, old_key}, state);

    m_tree->erase(ndx);

    if (recurse) {
        _impl::TableFriend::remove_recursive(*origin_table, state); // Throws
    }
    if (old_key.is_unresolved()) {
        // We might have removed the last unresolved link - check it

        // FIXME: Exploit the fact that the values are sorted and unresolved
        // keys have a negative value.
        _impl::check_for_last_unresolved(&tree());
    }
}

template <>
void Set<ObjKey>::do_clear()
{
    size_t ndx = size();
    while (ndx--) {
        do_erase(ndx);
    }

    m_tree->set_context_flag(false);
}

template <>
void Set<ObjKey>::migrate()
{
}

template class Set<ObjKey>;

template <>
void Set<ObjLink>::do_insert(size_t ndx, ObjLink target_link)
{
    m_obj.set_backlink(m_col_key, target_link);
    tree().insert(ndx, target_link);
}

template <>
void Set<ObjLink>::do_erase(size_t ndx)
{
    ObjLink old_link = get(ndx);
    CascadeState state(old_link.get_obj_key().is_unresolved() ? CascadeState::Mode::All : CascadeState::Mode::Strong);

    bool recurse = m_obj.remove_backlink(m_col_key, old_link, state);

    m_tree->erase(ndx);

    if (recurse) {
        auto table = m_obj.get_table();
        _impl::TableFriend::remove_recursive(*table, state); // Throws
    }
}

template <>
void Set<Mixed>::do_insert(size_t ndx, Mixed value)
{
    if (value.is_type(type_TypedLink)) {
        auto target_link = value.get<ObjLink>();
        m_obj.get_table()->get_parent_group()->validate(target_link);
        m_obj.set_backlink(m_col_key, target_link);
    }
    tree().insert(ndx, value);
}

template <>
void Set<Mixed>::do_erase(size_t ndx)
{
    if (Mixed old_value = get(ndx); old_value.is_type(type_TypedLink)) {
        auto old_link = old_value.get<ObjLink>();

        CascadeState state(old_link.get_obj_key().is_unresolved() ? CascadeState::Mode::All
                                                                  : CascadeState::Mode::Strong);
        bool recurse = m_obj.remove_backlink(m_col_key, old_link, state);

        m_tree->erase(ndx);

        if (recurse) {
            auto table = m_obj.get_table();
            _impl::TableFriend::remove_recursive(*table, state); // Throws
        }
    }
    else {
        m_tree->erase(ndx);
    }
}

template <>
void Set<Mixed>::do_clear()
{
    size_t ndx = size();
    while (ndx--) {
        do_erase(ndx);
    }
}

template <>
void Set<Mixed>::migrate()
{
    // We should just move all string values to be before the binary values
    size_t first_binary = size();
    for (size_t n = 0; n < size(); n++) {
        if (tree().get(n).is_type(type_Binary)) {
            first_binary = n;
            break;
        }
    }

    for (size_t n = first_binary; n < size(); n++) {
        if (tree().get(n).is_type(type_String)) {
            tree().insert(first_binary, Mixed());
            tree().swap(n + 1, first_binary);
            m_tree->erase(n + 1);
            first_binary++;
        }
    }
}

void LnkSet::remove_target_row(size_t link_ndx)
{
    // Deleting the object will automatically remove all links
    // to it. So we do not have to manually remove the deleted link
    ObjKey k = get(link_ndx);
    get_target_table()->remove_object(k);
}

void LnkSet::remove_all_target_rows()
{
    if (m_set.update()) {
        _impl::TableFriend::batch_erase_rows(*get_target_table(), m_set.tree());
    }
}

void set_sorted_indices(size_t sz, std::vector<size_t>& indices, bool ascending)
{
    indices.resize(sz);
    if (ascending) {
        std::iota(indices.begin(), indices.end(), 0);
    }
    else {
        std::iota(indices.rbegin(), indices.rend(), 0);
    }
}

template <typename Iterator>
static bool partition_points(const Set<Mixed>& set, std::vector<size_t>& indices, Iterator& first_string,
                             Iterator& first_binary, Iterator& end)
{
    first_string = std::partition_point(indices.begin(), indices.end(), [&](size_t i) {
        return set.get(i).is_type(type_Bool, type_Int, type_Float, type_Double, type_Decimal);
    });
    if (first_string == indices.end() || !set.get(*first_string).is_type(type_String))
        return false;
    first_binary = std::partition_point(first_string + 1, indices.end(), [&](size_t i) {
        return set.get(i).is_type(type_String);
    });
    if (first_binary == indices.end() || !set.get(*first_binary).is_type(type_Binary))
        return false;
    end = std::partition_point(first_binary + 1, indices.end(), [&](size_t i) {
        return set.get(i).is_type(type_Binary);
    });
    return true;
}

template <>
void Set<Mixed>::sort(std::vector<size_t>& indices, bool ascending) const
{
    set_sorted_indices(size(), indices, true);

    // The on-disk order is bool -> numbers -> string -> binary -> others
    // We want to merge the string and binary sections to match the sort order
    // of other collections. To do this we find the three partition points
    // where the first string occurs, the first binary occurs, and the first
    // non-binary after binaries occurs. If there's no strings or binaries we
    // don't have to do anything. If they're both non-empty, we perform an
    // in-place merge on the strings and binaries.
    std::vector<size_t>::iterator first_string, first_binary, end;
    if (partition_points(*this, indices, first_string, first_binary, end)) {
        std::inplace_merge(first_string, first_binary, end, [&](auto a, auto b) {
            return get(a) < get(b);
        });
    }
    if (!ascending) {
        std::reverse(indices.begin(), indices.end());
    }
}

} // namespace realm
