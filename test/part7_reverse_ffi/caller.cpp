extern "C" {
    int add(int a, int b);
    void greet(const char* name);
}

#include <iostream>

int main() {
    int total = add(10, 32);
    std::cout << "total=" << total << "\n";
    greet("ThaiThon");
    return total == 42 ? 0 : 1;
}
