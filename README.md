Pscal implements a substantial subset of the original Pascal language specification, along with a few extensions.  Notable things it does not support are nested functions/procedure and pointers.  It is also completely free of object oriented constructs by design.  Sorry, not a fan of that programming paradigm.

The most notable thing about pscal is that while I've been directing the development and the doing the debugging, the large majority of the code has been written by various AI's. Anyone who has tried to work on a medium to large sized project with AI will know that at least as of the time I'm writing this that is no easy task.  Limited context windows and other limitations quickly lead to unpredictable results and constant code breakage.  The OpenAI models have been especially bad at this, though I haven't tried o3 or o4-mini yet.

The bulk of my recent development has been by way of Google's Gemeni 2.5 Pro.  Which has for the most part been a breath of fresh air.  It's not perfect, but it's better than the vast majority of human programmers even at the current scale of pscal, which is about 8K lines as I write this README.

Pscal uses cmake, but I've only just started learning that particular tool.  I do my development on an M1 MacBook Pro, primarily using (shudder) Xcode.

As the code was written primarily by AI's I'm releasing this to the public domain via the 'unlicense".
