#ifndef TMP_HPP
#define TMP_HPP

#include "stdafx.hpp"
#include <type_traits>
#include <cstdint>

namespace tmp { // simple template meta progamming

template <int N> constexpr int log2() { static_assert(N != 0, "wrong argument for log2"); return log2<N/2>() + 1; }
template <> constexpr int log2<1>() { return 0; }
template <typename T> constexpr int log2bitsof() { return log2<sizeof(T)*8>(); }

template <int bits, bool sign> struct integer { /*typedef char type;*/ };
template <> struct integer<16, true>  { typedef std::int16_t  type; };
template <> struct integer<32, true>  { typedef std::int32_t  type; };
template <> struct integer<64, true>  { typedef std::int64_t  type; };
template <> struct integer<16, false> { typedef std::uint16_t type; };
template <> struct integer<32, false> { typedef std::uint32_t type; };
template <> struct integer<64, false> { typedef std::uint64_t type; };

template <int bits> struct floating_point { typedef char type; };
template <> struct floating_point<32> { typedef float  type; };
template <> struct floating_point<64> { typedef double type; };

template<class... Args> static inline void pass(const Args &...) { }

template<int idx, int size, bool go = idx < size>
struct static_for { };

template<int idx, int size>
struct static_for<idx, size, true> {
	template<class... Args, class F>
	static inline void run(const std::tuple<Args...> &tuple, const F &func) {
		func(std::get<idx>(tuple));
		static_for<idx+1, size>::run(tuple, func);
	}
};

template<int idx, int size>
struct static_for<idx, size, false> {
	template<class T, class F>
	static inline void run(const T &, const F &) { }
	template<class T, class F>
	static inline void run(T &&, const F &) { }
};

template<int... S>
struct index_list {
	template<int n>
	constexpr auto added() const -> decltype(index_list<S+n...>()) {
		return index_list<S+n...>();
	}
	template<int n>
	constexpr auto multiplied() const -> decltype(index_list<S+n...>()) {
		return index_list<S*n...>();
	}
};

namespace detail {

template<int diff, int N, int... S>
struct index_list_generate_interval : public index_list_generate_interval<diff, N-diff, N-diff, S...> { };

template<int diff, int ...S>
struct index_list_generate_interval<diff, 0, S...> { typedef index_list<S...> type; };

template<class F, class... Args, int... I>
static inline void call_with_tuple_impl(F &&func, std::tuple<Args...> &&tuple, index_list<I...>) {
	func(std::get<I>(tuple)...);
}

template<class F, class... Args, int... I>
static inline void call_with_tuple_impl(F &&func, std::tuple<Args...> &tuple, index_list<I...>) {
	func(std::get<I>(tuple)...);
}

template<class F, class... Args, int... I>
static inline void call_with_tuple_impl(F &&func, const std::tuple<Args...> &tuple, index_list<I...>) {
	func(std::get<I>(tuple)...);
}

}

template<int N, int interval = 1>
static inline auto make_tuple_index()
		-> decltype(typename detail::index_list_generate_interval<interval, N%interval == 0 ? N : (N/interval+1)*interval>::type()) {
	return typename detail::index_list_generate_interval<interval, N%interval == 0 ? N : (N/interval+1)*interval>::type();
}

template<int interval = 1, class... Args>
static inline auto make_tuple_index(const std::tuple<Args...> &) -> decltype(make_tuple_index<sizeof...(Args), interval>()) {
	return make_tuple_index<sizeof...(Args), interval>();
}

template<class... Args, int... I>
static inline auto extract_tuple(const std::tuple<Args...> &tuple, index_list<I...>) -> decltype(std::tie(std::get<I>(tuple)...)) {
	return std::tie(std::get<I>(tuple)...);
}

template<class F, class... Args, int... I>
static inline void call_with_tuple(const F &func, std::tuple<Args...> &&tuple, index_list<I...> index) {
	detail::call_with_tuple_impl(func, tuple, index);
}

template<class F, class... Args, int... I>
static inline void call_with_tuple(const F &func, std::tuple<Args...> &tuple, index_list<I...> index) {
	detail::call_with_tuple_impl(func, tuple, index);
}

template<class F, class... Args, int... I>
static inline void call_with_tuple(const F &func, const std::tuple<Args...> &tuple, index_list<I...> index) {
	detail::call_with_tuple_impl(func, tuple, index);
}

template<class F, class... Args>
static inline void call_with_tuple(const F &func, std::tuple<Args...> &&tuple) {
	detail::call_with_tuple_impl(func, tuple, make_tuple_index(tuple));
}

template<class F, class... Args>
static inline void call_with_tuple(const F &func, std::tuple<Args...> &tuple) {
	detail::call_with_tuple_impl(func, tuple, make_tuple_index(tuple));
}

template<class F, class... Args>
static inline void call_with_tuple(const F &func, const std::tuple<Args...> &tuple) {
	detail::call_with_tuple_impl(func, tuple, make_tuple_index(tuple));
}

template<class... Args>
static inline constexpr int tuple_size(const std::tuple<Args...> &) { return sizeof...(Args); }

namespace detail {

template<int n, int size>
struct for_each_tuple_impl {
	template<class F, class... Args>
	static inline void run(const std::tuple<Args...> &tuple, F &&func) {
		func(std::get<n>(tuple));
		for_each_tuple_impl<n+1, size>::run(tuple, std::forward<F>(func));
	}
	template<class F, class... Args1, class... Args2>
	static inline void run(const std::tuple<Args1...> &tuple1, const std::tuple<Args2...> &tuple2, F &&func) {
		static_assert(sizeof...(Args1) == sizeof...(Args2), "tuple size does not match");
		func(std::get<n>(tuple1), std::get<n>(tuple2));
		for_each_tuple_impl<n+1, size>::run(tuple1, tuple2, std::forward<F>(func));
	}
};

template<int size>
struct for_each_tuple_impl<size, size> {
	template<class... Args> static inline void run(Args&... ) {}
	template<class... Args> static inline void run(const Args&... ) {}
	template<class... Args> static inline void run(Args&&... ) {}
};

struct make_json_impl {
	make_json_impl(QJsonObject &json): m_json(json) {}
	void operator () (const QString &key, const QJsonValue &value) const {
		m_json.insert(key, value);
	}
	void operator () (const char *key, const QJsonValue &value) const {
		m_json.insert(_L(key), value);
	}
	void operator () (const char *key, const char *value) const {
		m_json.insert(_L(key), _L(value));
	}
private:
	QJsonObject &m_json;
};

}

template<class F, class... Args>
void for_each(const std::tuple<Args...> &tuple, const F &func) {
	detail::for_each_tuple_impl<0, sizeof...(Args)>::run(tuple, func);
}

template<class F, class... Args1, class... Args2>
void for_each(const std::tuple<Args1...> &tuple1, const std::tuple<Args2...> &tuple2, F &&func) {
	detail::for_each_tuple_impl<0, sizeof...(Args1)>::run(tuple1, tuple2, std::forward<F>(func));
}

template<class... Args>
static QJsonObject make_json(const Args &... args) {
	const auto params = std::tie(args...);
	const auto keyIdx = tmp::make_tuple_index<2>(params);
	const auto valueIdx = keyIdx.template added<1>();
	const auto keys = tmp::extract_tuple(params, keyIdx);
	const auto values = tmp::extract_tuple(params, valueIdx);
	QJsonObject json;
	for_each(keys, values, detail::make_json_impl(json));
	return json;
}

}


#endif // TMP_HPP
