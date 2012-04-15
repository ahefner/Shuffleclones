Conservative C++ version (ISO C++ 2003)
=======================================

This is a straightforward C++03 implementation, without Boost or C++11 features. It takes good advantage of STL containers and RAII, probably to a fault: my choices favored clarity over efficiency, so there's some redundant copying of strings and vectors. It's fairly relaxed about access rights and encapsulation, except in a few important places (so that one cannot accidentally circumvent the locks protecting state owned by particular threads, for instance). It isn't the strictest, most straight-laced textbook C++, but the style is perfectly reasonable, particularly at this scale (one developer, small program).

