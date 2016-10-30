# async
This is an experimental async concept using `switch` statements to create resumable functions that are not very different from their simplest synchronous counterparts.

## switch abuse
Traditionally `switch` is used and taught as little more than a look-up table for code, a way to clean up and speed up several `if` tests against a single value.

This model takes fuller advantage of `switch`, which allows the user to place case labels deep inside nested code:
```
switch(x)
{
case 1:
   for(i = 0; i < 100; ++i)
   {
   case 2:
      printf("loop %d\n", i);
   }
}
```
Please don't do this. It's ugly and rarely a good use. This is just a concept, remember.

## resumable functions
Using the capability of `switch` above, we have resumable functions:
```
// these variables exist outside of our function,
// because the function will potentially be called multiple times.
enum { on_start, on_loop } state = on_start;
int i;

// inside the function.
switch(state)
{
case on_start:
   puts("starting...");
   
   for(i = 0; i < 100; ++i)
   {
      if((i % 2) == 0)
      {
         state = on_loop; // set our resume point.
         return; // yield our time in the function by returning.
      case on_loop: // on next call, we resume from here.
      }
      
      printf("loop %d\n", i);
   }
}
```

## async model
### fast synchronous calls
This recognizes that async functions -- especially on low-latency or highly loaded servers -- will occasionally be able to complete synchronously. Many async libraries would have you process a callback even in this case, but that is very bad for performance. So, every async function has the signature of:
```
bool async_func()
```
If it returns true, it completed synchronously. If false, it's still running. When it returns false, the caller is expected to yield their time.

### context tracking
Async functions do have some state to track, though. Many libraries will allocate this state in the background and while modern allocators are well suited for frequent allocations of similar sizes, it's always going to be more performant to invoke the allocator as little as possible. The caller is expected to allocate this state as part of its own state:
```
bool async_func(socket_context &ctx);

struct my_op
{
   async_context<socket_context> ctx; // pre-allocate the context for the function here.
   
   void step()
   {
      async_func(ctx); // call it with its context.
      
      // when the op is completed, this will cause any pending exceptions to be thrown.
      // or, you can manually check the status if you don't want an exception.
      // any other results from the operation are similarly inside of the context.
      ctx.check_errors();
   }
};
```

### async operations
Tying it all together is the `async_op` type: it defines a resumable function that is called by `async_context` when an async function completes. This is where all of the state for the parent (compound) operation goes:
```
struct my_op : public async_op
{
   async_context<connect_context> connect_ctx;
   async_context<socket_context> read_ctx;
   socket sock;
   enum { on_start, on_connected, on_read } state;
   
   my_op()
      : connect_ctx(this) // the contexts need to know which `async_op` to resume on.
      , read_ctx(this)
      , state(on_start)
   {
   }
   
   void step() // this is a virtual method.
   {
      switch(state)
      {
      case on_start:
         // connect our socket.
         
         state = on_connected;
         if(!sock.connect(connect_ctx))
            return;
            
      case on_connected:
         connect_ctx.check_errors();
         
         do
         {
            // loop through reads until nothing is left.
            
            state = on_read;
            if(!sock.read(read_context))
               return;
               
         case on_read:
            read_context.check_errors();
         } while(read_context.transferred() != 0);
   }
};
```

A complete example can be seen in main.cpp, which calls up the google homepage.

### Where to go from here

As with most async models, a neccesary evil behind the scenes is that an I/O completion must go through two or more callbacks before reaching your own code depending on how nested the operation is. I've tested a model that uses CRTP instead, which has the benefit of being as efficient as hand-written code: only a single non-inlineable callback every time. Unfortunately it has its own challenges keeping user code clean, so I've tabled it for now until I have more time to think about it.

I'm still not sure if `switch` is the best way to use async. Although compiler-generated async functions will likely generate state machines identical to this, hand-writing them has some downfalls beyond simple readability. Variable initialization is a bit of a pain inside of switch statements, and having such a close look to synchronous code leaves things a bit open to error if variables aren't hoisted properly. I imagine it may also become a bit unweildy if you have your own switch statements inside of the async `switch`.
