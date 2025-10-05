#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerOpenAIChatCompletionsBuiltin(void);

void registerOpenAIBuiltins(void) {
    const char *category = "openai";
    extBuiltinRegisterCategory(category);
    extBuiltinRegisterGroup(category, "chat");
    extBuiltinRegisterFunction(category, "chat", "OpenAIChatCompletions");
    registerOpenAIChatCompletionsBuiltin();
}
