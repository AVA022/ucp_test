#include <memory>
#include <iostream>
#include <string>

struct MyClass {
    int value;
};

int main() {
    int num = 2;

    size_t size = (size_t)num;
    std::cout << size << std::endl;

    return 0;
}