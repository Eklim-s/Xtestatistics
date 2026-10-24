#pragma once
#include <memory>
#include <new>
#include <type_traits>
template <class T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  return std::unique_ptr<T>(new (std::nothrow) std::remove_extent_t<T>[count]);
}
