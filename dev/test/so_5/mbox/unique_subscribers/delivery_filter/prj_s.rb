require 'mxx_ru/cpp'

MxxRu::Cpp::exe_target {

	required_prj 'so_5/prj_s.rb'

	target '_unit.test.so_5.mbox.unique_subscribers.delivery_filter_s'

	cpp_source 'main.cpp'
}

