lisp-interpreter
===============

## About

An embeddable lisp interepreter written in C. I created this while reading [SICP](https://github.com/justinmeiners/sicp-excercises) to improve my knowledge of lisp and to make an implementation that allows me to easily add scripting to my own programs.


### Philosophy

- **Simple**: An interpreter is something that can easily get complicated with fancy features. This project doesn't aim to be an optimal, fully featured, or standards compliant Scheme implementation. It is just a robust foundation for scripting. (If you need a more complete implementation try [s7](https://ccrma.stanford.edu/software/snd/snd/s7.html))
- **Data & Code**: Lisp is undervalued as an alternative to JSON or XML. This implementation provides first-class support for working with data or code.
- **Unintrusive**: Just copy in the header and source file. Source code should be portable between major platforms.
- **Unsurprising**: You should be able to read the source code and understand how it works. The header API should work how you expect.

### Features

- Scheme-like (but not confined to) syntax. if, let, and, or, etc.
- Closures
- Exact [garbage collection](#garbage-collection) with explicit invocation.
- Symbol table
- Easy integration of C functions.
- REPL command line tool.
- Data loading and manipulation.
- Single header and source file.

## Examples


### Interactive programming with Read, eval, print loop.
```bash
$ ./lisp_i
> (define (sqr x) (* x x)))
> (define length 40)
> (define area 0)
> (set! area (sqr length))
1600
```

### Embedding in a program

```c
// setup lisp with 1 MB of heap
LispContext ctx = lisp_init_interpreter();

// load lisp program (add 1 and 2)
LispError error;
Lisp program = lisp_read_expand("(+ 1 2)", &error, ctx);

// execute program using global environment
Lisp result = lisp_eval_global(program, &error, ctx);

// prints 3
lisp_print(result);

// you are responsible for garbage collection
lisp_collect(ctx, env);
// ...
// shutdown also garbage collects
lisp_shutdown(ctx, env);
```

### Loading Data

Lisp s-expressions can be used as a lightweight substitute to JSON or XML.

JSON
```json
{
   "name" : "bob jones",
   "age" : 54,
   "city" : "SLC",
}
```

Lisp
```scheme
#((name . "bob jones")
    (age . 54)
    (city . "SLC"))
```
Loading the structure in C.

```c
// setup lisp with 1 MB of heap
LispContext ctx = lisp_init_empty();
// load lisp structure
Lisp data = lisp_read_file(file, ctx);
// get value for age
Lisp age = lisp_vector_for_key(data, lisp_make_symbol("AGE", ctx), ctx);
// ...
lisp_shutdown(ctx);
```

### Calling C functions

C functions can be used to extend the interpreter, or call into C code.

```c
Lisp sum_of_squares(Lisp args, LispError* e, LispContext ctx)
{
    // first argument
    Lisp accum = lisp_car(args);
    // remaining arguments
    Lisp rest = lisp_cdr(args);

    // iterate over the rest of the arguments
    while (!lisp_is_null(rest))
    {
        Lisp val = lisp_car(rest);

        // make this work for integers and floats
        if (lisp_type(accum) == LISP_INT)
        {
            accum.int_val += lisp_int(val) * lisp_int(val);
        }
        else if (lisp_type(accum) == LISP_FLOAT)
        {
            accum.float_val += lisp_float(val) * lisp_float(val);
        }

        rest = lisp_cdr(rest);
    }

    return accum;
}

// wrap in Lisp object
Lisp func = lisp_make_func(sum_of_squares);

// add to enviornment with symbol SUM-OF-SQUARES
lisp_env_set(env, lisp_make_symbol("SUM-OF-SQAURES", ctx), func, ctx);
```

In Lisp
```scheme
(sum-of-squares 1 2 3)
; returns 1 + 4 + 9 = 14
```
Constants can also be stored in the environment in a similar fashion.

```c
Lisp pi = lisp_make_float(3.1415);
lisp_env_set(env, lisp_make_symbol("PI", ctx), pi, ctx);
```

## Garbage Collection

The lisp interpreter uses the [Cheney algorithim](https://en.wikipedia.org/wiki/Cheney%27s_algorithm) for garbage collection.

Memory is allocated in fixed size pages. When an allocation is request and the current page does not have enough space remaining, a new page will be allocated to fulfill the allocation. So, allocations will continue to use up more memory until garbage collection is invoked by calling `lisp_collect`. Note that tail call recursion will not overflow the C stack, but will use additional memory for each function call.

The choice to use explicit, rather than automatic garbage collection, was made so that the interpreter does not need to keep track of every lisp object on the stack, only the most important objects. If garbage collection was allowed to trigger in the middle of a C function call, then the interpreter would need to be able to "see" all the lisp values on the call stack, in order to prevent them from being collected. Providing this feature would make integrating with C code much more complicated and conflict with the project's goal of being easily embeddable.

This means that when `lisp_collect` is called, all lisp values which are not reachable from the global environment or the function's parameters become invalidated. Be conscious of when and where you call the garbage collector.

You can learn about an alternative solution in the [Lua Scripting Language](https://www.lua.org/pil/24.2.html).

## Project License

Copyright (c) 2018 Justin Meiners

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

