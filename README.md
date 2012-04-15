Shuffleclones: Comparative Shuffletron
======================================

This respository is intended to collect simple reimplementations of a subset of the Shuffletron music player (originally written in Common Lisp) in various languages.

Presently, only implementations in C++ are done. In the future, a feature-comparable subset of the Lisp version should be extracted for comparison. In the mean time, the definitive Lisp version of Shuffletron can be found at https://github.com/ahefner/shuffletron

I'm keeping these in one admittedly long source file rather then breaking them up, so that the program can be read straight through top to bottom. It's just small enough that this isn't unreasonable, and should remain so.

I'm not sure how far too take the feature set of these versions. I think the C++ version has gone too far, but far enough that it needs to be taken slightly farther to its logical conclusion (adding Shuffletron-style colored song listings).

I'm planning to do a straight C version, at some point. This will be interesting: no RAII and no STL containers, so custom data structures and probably more UNIX-centric.
