/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
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


#include "realm/list.hpp"
#include "realm/cluster_tree.hpp"
#include "realm/array_basic.hpp"
#include "realm/array_integer.hpp"
#include "realm/array_bool.hpp"
#include "realm/array_string.hpp"
#include "realm/array_binary.hpp"
#include "realm/array_timestamp.hpp"
#include "realm/array_decimal128.hpp"
#include "realm/array_fixed_bytes.hpp"
#include "realm/array_typed_link.hpp"
#include "realm/array_mixed.hpp"
#include "realm/column_type_traits.hpp"
#include "realm/object_id.hpp"
#include "realm/table.hpp"
#include "realm/table_view.hpp"
#include "realm/group.hpp"
#include "realm/replication.hpp"

namespace realm {

// FIXME: This method belongs in obj.cpp.
LstBasePtr Obj::get_listbase_ptr(ColKey col_key) const
{
    auto attr = get_table()->get_column_attr(col_key);
    REALM_ASSERT(attr.test(col_attr_List));
    bool nullable = attr.test(col_attr_Nullable);

    switch (get_table()->get_column_type(col_key)) {
        case type_Int: {
            if (nullable)
                return std::make_unique<Lst<util::Optional<Int>>>(*this, col_key);
            else
                return std::make_unique<Lst<Int>>(*this, col_key);
        }
        case type_Bool: {
            if (nullable)
                return std::make_unique<Lst<util::Optional<Bool>>>(*this, col_key);
            else
                return std::make_unique<Lst<Bool>>(*this, col_key);
        }
        case type_Float: {
            if (nullable)
                return std::make_unique<Lst<util::Optional<Float>>>(*this, col_key);
            else
                return std::make_unique<Lst<Float>>(*this, col_key);
        }
        case type_Double: {
            if (nullable)
                return std::make_unique<Lst<util::Optional<Double>>>(*this, col_key);
            else
                return std::make_unique<Lst<Double>>(*this, col_key);
        }
        case type_String: {
            return std::make_unique<Lst<String>>(*this, col_key);
        }
        case type_Binary: {
            return std::make_unique<Lst<Binary>>(*this, col_key);
        }
        case type_Timestamp: {
            return std::make_unique<Lst<Timestamp>>(*this, col_key);
        }
        case type_Decimal: {
            return std::make_unique<Lst<Decimal128>>(*this, col_key);
        }
        case type_ObjectId: {
            if (nullable)
                return std::make_unique<Lst<util::Optional<ObjectId>>>(*this, col_key);
            else
                return std::make_unique<Lst<ObjectId>>(*this, col_key);
        }
        case type_UUID: {
            if (nullable)
                return std::make_unique<Lst<util::Optional<UUID>>>(*this, col_key);
            else
                return std::make_unique<Lst<UUID>>(*this, col_key);
        }
        case type_TypedLink: {
            return std::make_unique<Lst<ObjLink>>(*this, col_key);
        }
        case type_Mixed: {
            return std::make_unique<Lst<Mixed>>(*this, col_key);
        }
        case type_LinkList:
            return get_linklist_ptr(col_key);
        case type_Link:
            break;
    }
    REALM_TERMINATE("Unsupported column type");
}

/****************************** Lst aggregates *******************************/

namespace {
void do_sort(std::vector<size_t>& indices, size_t size, util::FunctionRef<bool(size_t, size_t)> comp)
{
    auto old_size = indices.size();
    indices.reserve(size);
    if (size < old_size) {
        // If list size has decreased, we have to start all over
        indices.clear();
        old_size = 0;
    }
    for (size_t i = old_size; i < size; i++) {
        // If list size has increased, just add the missing indices
        indices.push_back(i);
    }

    auto b = indices.begin();
    auto e = indices.end();
    std::sort(b, e, comp);
}
} // anonymous namespace

template <class T>
void Lst<T>::sort(std::vector<size_t>& indices, bool ascending) const
{
    update_if_needed();

    auto tree = m_tree.get();
    if (ascending) {
        do_sort(indices, size(), [tree](size_t i1, size_t i2) {
            return unresolved_to_null(tree->get(i1)) < unresolved_to_null(tree->get(i2));
        });
    }
    else {
        do_sort(indices, size(), [tree](size_t i1, size_t i2) {
            return unresolved_to_null(tree->get(i1)) > unresolved_to_null(tree->get(i2));
        });
    }
}

// std::unique, but leaving the minimum value rather than the first found value
// for runs of duplicates. This makes distinct stable without relying on a
// stable sort, which makes it easier to write tests and avoids surprising results
// where distinct appears to change the order of elements
template <class Iterator, class Predicate>
static Iterator min_unique(Iterator first, Iterator last, Predicate pred)
{
    if (first == last) {
        return first;
    }

    Iterator result = first;
    while (++first != last) {
        bool equal = pred(*result, *first);
        if ((equal && *result > *first) || (!equal && ++result != first))
            *result = *first;
    }
    return ++result;
}

template <class T>
void Lst<T>::distinct(std::vector<size_t>& indices, util::Optional<bool> sort_order) const
{
    indices.clear();
    sort(indices, sort_order.value_or(true));
    if (indices.empty()) {
        return;
    }

    auto tree = m_tree.get();
    auto duplicates = min_unique(indices.begin(), indices.end(), [tree](size_t i1, size_t i2) noexcept {
        return unresolved_to_null(tree->get(i1)) == unresolved_to_null(tree->get(i2));
    });

    // Erase the duplicates
    indices.erase(duplicates, indices.end());

    if (!sort_order) {
        // Restore original order
        std::sort(indices.begin(), indices.end());
    }
}

/********************************* Lst<Key> *********************************/

template <>
void Lst<ObjKey>::do_set(size_t ndx, ObjKey target_key)
{
    auto origin_table = m_obj.get_table();
    auto target_table_key = origin_table->get_opposite_table_key(m_col_key);
    ObjKey old_key = this->get(ndx);
    CascadeState state(CascadeState::Mode::Strong);
    bool recurse =
        m_obj.replace_backlink(m_col_key, {target_table_key, old_key}, {target_table_key, target_key}, state);

    m_tree->set(ndx, target_key);

    if (recurse) {
        _impl::TableFriend::remove_recursive(*origin_table, state); // Throws
    }
    if (target_key.is_unresolved()) {
        if (!old_key.is_unresolved())
            m_tree->set_context_flag(true);
    }
    else if (old_key.is_unresolved()) {
        // We might have removed the last unresolved link - check it
        _impl::check_for_last_unresolved(m_tree.get());
    }
}

template <>
void Lst<ObjKey>::do_insert(size_t ndx, ObjKey target_key)
{
    auto origin_table = m_obj.get_table();
    auto target_table_key = origin_table->get_opposite_table_key(m_col_key);
    m_obj.set_backlink(m_col_key, {target_table_key, target_key});
    m_tree->insert(ndx, target_key);
    if (target_key.is_unresolved()) {
        m_tree->set_context_flag(true);
    }
}

template <>
void Lst<ObjKey>::do_remove(size_t ndx)
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
        _impl::check_for_last_unresolved(m_tree.get());
    }
}

template <>
void Lst<ObjKey>::do_clear()
{
    auto origin_table = m_obj.get_table();
    TableRef target_table = m_obj.get_target_table(m_col_key);

    size_t sz = size();
    if (!target_table->is_embedded()) {
        size_t ndx = sz;
        while (ndx--) {
            do_set(ndx, null_key);
            m_tree->erase(ndx);
        }
        m_tree->set_context_flag(false);
        return;
    }

    TableKey target_table_key = target_table->get_key();
    ColKey backlink_col = origin_table->get_opposite_column(m_col_key);

    CascadeState state;

    typedef _impl::TableFriend tf;
    for (size_t ndx = 0; ndx < sz; ++ndx) {
        ObjKey target_key = m_tree->get(ndx);
        Obj target_obj = target_table->get_object(target_key);
        target_obj.remove_one_backlink(backlink_col, m_obj.get_key()); // Throws
        // embedded objects should only have one incoming link
        REALM_ASSERT_EX(target_obj.get_backlink_count() == 0, target_obj.get_backlink_count());
        state.m_to_be_deleted.emplace_back(target_table_key, target_key);
    }

    m_tree->clear();
    m_tree->set_context_flag(false);

    tf::remove_recursive(*origin_table, state); // Throws
}

template <>
void Lst<ObjLink>::do_set(size_t ndx, ObjLink target_link)
{
    ObjLink old_link = get(ndx);
    CascadeState state(old_link.get_obj_key().is_unresolved() ? CascadeState::Mode::All : CascadeState::Mode::Strong);
    bool recurse = m_obj.replace_backlink(m_col_key, old_link, target_link, state);

    m_tree->set(ndx, target_link);

    if (recurse) {
        auto origin_table = m_obj.get_table();
        _impl::TableFriend::remove_recursive(*origin_table, state); // Throws
    }
}

template <>
void Lst<ObjLink>::do_insert(size_t ndx, ObjLink target_link)
{
    m_obj.set_backlink(m_col_key, target_link);
    m_tree->insert(ndx, target_link);
}

template <>
void Lst<ObjLink>::do_remove(size_t ndx)
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
void Lst<Mixed>::do_set(size_t ndx, Mixed value)
{
    ObjLink old_link;
    ObjLink target_link;
    Mixed old_value = m_tree->get(ndx);

    if (old_value.is_type(type_TypedLink)) {
        old_link = old_value.get<ObjLink>();
    }
    if (value.is_type(type_TypedLink)) {
        target_link = value.get<ObjLink>();
        m_obj.get_table()->get_parent_group()->validate(target_link);
    }

    CascadeState state(old_link.get_obj_key().is_unresolved() ? CascadeState::Mode::All : CascadeState::Mode::Strong);
    bool recurse = m_obj.replace_backlink(m_col_key, old_link, target_link, state);

    m_tree->set(ndx, value);

    if (recurse) {
        auto origin_table = m_obj.get_table();
        _impl::TableFriend::remove_recursive(*origin_table, state); // Throws
    }
}

template <>
void Lst<Mixed>::do_insert(size_t ndx, Mixed value)
{
    if (value.is_type(type_TypedLink)) {
        m_obj.set_backlink(m_col_key, value.get<ObjLink>());
    }
    m_tree->insert(ndx, value);
}

template <>
void Lst<Mixed>::do_remove(size_t ndx)
{
    if (Mixed old_value = m_tree->get(ndx); old_value.is_type(type_TypedLink)) {
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
void Lst<Mixed>::do_clear()
{
    size_t ndx = size();
    while (ndx--) {
        do_remove(ndx);
    }
}

Obj LnkLst::create_and_insert_linked_object(size_t ndx)
{
    Table& t = *get_target_table();
    auto o = t.is_embedded() ? t.create_linked_object() : t.create_object();
    m_list.insert(ndx, o.get_key());
    return o;
}

Obj LnkLst::create_and_set_linked_object(size_t ndx)
{
    Table& t = *get_target_table();
    auto o = t.is_embedded() ? t.create_linked_object() : t.create_object();
    m_list.set(ndx, o.get_key());
    return o;
}

TableView LnkLst::get_sorted_view(SortDescriptor order) const
{
    TableView tv(clone_linklist());
    tv.do_sync();
    tv.sort(std::move(order));
    return tv;
}

TableView LnkLst::get_sorted_view(ColKey column_key, bool ascending) const
{
    TableView v = get_sorted_view(SortDescriptor({{column_key}}, {ascending}));
    return v;
}

void LnkLst::remove_target_row(size_t link_ndx)
{
    // Deleting the object will automatically remove all links
    // to it. So we do not have to manually remove the deleted link
    ObjKey k = get(link_ndx);
    get_target_table()->remove_object(k);
}

void LnkLst::remove_all_target_rows()
{
    if (is_attached()) {
        update_if_needed();
        _impl::TableFriend::batch_erase_rows(*get_target_table(), *m_list.m_tree);
    }
}

void LnkLst::replace_link(ObjKey old_val, ObjKey new_val)
{
    update_if_needed();
    auto tree = m_list.m_tree.get();
    auto n = tree->find_first(old_val);
    REALM_ASSERT(n != realm::npos);
    if (Replication* repl = get_obj().get_replication()) {
        repl->list_set(m_list, n, new_val);
    }
    tree->set(n, new_val);
    m_list.bump_content_version();
    if (new_val.is_unresolved()) {
        if (!old_val.is_unresolved()) {
            tree->set_context_flag(true);
        }
    }
    else {
        _impl::check_for_last_unresolved(tree);
    }
}

// Force instantiation:
template class Lst<ObjKey>;
template class Lst<Mixed>;
template class Lst<ObjLink>;
template class Lst<int64_t>;
template class Lst<bool>;
template class Lst<StringData>;
template class Lst<BinaryData>;
template class Lst<Timestamp>;
template class Lst<float>;
template class Lst<double>;
template class Lst<Decimal128>;
template class Lst<ObjectId>;
template class Lst<UUID>;
template class Lst<util::Optional<int64_t>>;
template class Lst<util::Optional<bool>>;
template class Lst<util::Optional<float>>;
template class Lst<util::Optional<double>>;
template class Lst<util::Optional<ObjectId>>;
template class Lst<util::Optional<UUID>>;

} // namespace realm
