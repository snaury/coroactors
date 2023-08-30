#pragma once
#include <stdexcept>

class no_value_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;

    no_value_error()
        : runtime_error("resumed without value")
    {}
};
