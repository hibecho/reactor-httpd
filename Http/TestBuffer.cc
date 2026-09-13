#include <iostream>
#include "Buffer.hpp"

int main()
{
    Buffer b;
    auto ptr = b.Begin();
    std::cout << *ptr << std::endl;
    return 0;
}