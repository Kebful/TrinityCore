/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TRINITY_DUMMYDATA_H
#define TRINITY_DUMMYDATA_H

#include "Common.h"
#include "Define.h"
#include "DBCStore.h"

#include <deque>
#include <list>
#include <string_view>

struct ItemTemplate;

class SpellInfo;

class UnitTestDataLoader
{
    public:
        template <typename T, uint32 T::*ID>
        class DBC
        {
            class LoaderGuard
            {
                public:
                    LoaderGuard(DBC& d) : _d(d) {}
                    ~LoaderGuard() { _d.Dump(); }

                    T& Add() { return _d._storage.emplace_back(); }
                private:
                    DBC& _d;
            };

            public:
                DBC(DBCStorage<T>& store) : _store(store) {}
                LoaderGuard Loader() { return {*this}; }
                void Dump()
                {
                    delete[] _store._indexTable.AsT;
                    for (T const& entry : _storage)
                        if (entry.*ID >= _store._indexTableSize)
                            _store._indexTableSize = entry.*ID + 1;
                    _store._indexTable.AsT = new T*[_store._indexTableSize];
                    for (size_t i = 0; i < _store._indexTableSize; ++i)
                        _store._indexTable.AsT[i] = nullptr;
                    for (T& entry : _storage)
                        _store._indexTable.AsT[entry.*ID] = &entry;
                }

                bool Empty() const
                {
                    return !_store._indexTable.AsT;
                }

            private:
                std::list<T> _storage;
                DBCStorage<T>& _store;
        };

        static void LoadAchievementTemplates();
        static void LoadItemTemplates();
        static void LoadSpellInfo();

        // Loader for a DBC whose file format has no id column (gtCombatRatings is
        // "f", gtOCTClassCombatRatingScalar is "df"; 'd' marks the sorted column,
        // which is not part of the entry). DBCStorage indexes those files by record
        // position -- the loader assigns ids 0..recordCount-1 in file order -- so a
        // test that wants the shipped row at a given id places it at that position.
        // Positions below the highest one asked for are zero-filled; only the rows a
        // test names are valid DBC data, so do not read any other id from the store.
        template <typename T>
        class FlatDBC
        {
            class LoaderGuard
            {
                public:
                    LoaderGuard(FlatDBC& d) : _d(d) {}
                    ~LoaderGuard() { _d.Dump(); }

                    T& At(uint32 position)
                    {
                        while (_d._storage.size() <= position)
                            _d._storage.push_back(T{});
                        return _d._storage[position];
                    }
                private:
                    FlatDBC& _d;
            };

            public:
                FlatDBC(DBCStorage<T>& store) : _store(store) {}
                LoaderGuard Loader() { return {*this}; }

                void Dump()
                {
                    delete[] _store._indexTable.AsT;
                    _store._indexTableSize = uint32(_storage.size());
                    _store._indexTable.AsT = new T*[_store._indexTableSize];
                    for (uint32 i = 0; i < _store._indexTableSize; ++i)
                        _store._indexTable.AsT[i] = &_storage[i];
                }

                bool Empty() const
                {
                    return !_store._indexTable.AsT;
                }

            private:
                // deque, not vector: At() hands out a reference that the caller fills
                // before the next At(), and deque keeps existing elements in place.
                std::deque<T> _storage;
                DBCStorage<T>& _store;
        };

    private:
        static ItemTemplate& GetItemTemplate(uint32 id, std::string_view name);
        static void SetItemLocale(uint32 id, LocaleConstant locale, std::string_view name);
};

#endif
