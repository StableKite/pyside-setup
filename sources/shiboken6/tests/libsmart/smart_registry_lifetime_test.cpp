// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
// A test registry must count live C++ objects independently of copy/move elision.
#include "smart_integer.h"
#include "smart_registry.h"
#include <iostream>
#include <utility>
int main()
{
    auto *registry=Registry::getInstance();int failures=0;
    auto check=[&](const char *phase,int expected) {
        const int actual=registry->countIntegers();
        std::cout<<"{\"phase\":\""<<phase<<"\",\"expected_live_cpp_objects\":"<<expected<<",\"actual_registry_count\":"<<actual<<"}"<<std::endl;
        if(actual!=expected)++failures;
    };
    check("initial",0);
    { Integer first,second;check("two_constructed",2);second=first;check("copy_assignment",2); }
    check("after_copy_scope",0);
    { Integer first;first=first;check("self_assignment",1); }
    check("after_self_scope",0);
    { Integer first;Integer second(std::move(first));check("move_constructor",2); }
    check("after_move_scope",0);
    { Integer first,second;second=std::move(first);check("move_assignment",2); }
    check("final",0);
    return failures==0?0:1;
}
