#include <stdio.h>
#include <quickjs.h>
#include <quickjs-libc.h>
#include <string.h>


static JSValue js_print(JSContext *ctx, JSValueConst /*this_val*/,
                        int argc, JSValueConst *argv)
{
    int i;
    const char *str;

    for(i = 0; i < argc; i++) {
        if (i != 0)
            putchar(' ');
        str = JS_ToCString(ctx, argv[i]);
        if (!str)
            return JS_EXCEPTION;
        fputs(str, stdout);
        JS_FreeCString(ctx, str);
    }
    putchar('\n');
    return JS_UNDEFINED;
}

int main() {
    printf("QuickJS - Node-API \n");

    JSRuntime* rt;
    JSContext* ctx;

    rt = JS_NewRuntime();
    ctx = JS_NewContextRaw(rt);

    JS_AddIntrinsicBaseObjects(ctx);
    JS_AddIntrinsicEval(ctx);

    JSValue global, console;

    global = JS_GetGlobalObject(ctx);

    console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_print, "log", 1));
    JS_SetPropertyStr(ctx, global, "console", console);

    JS_FreeValue(ctx, global);

    const char* source =  "console.log(Math.pow(12, 2));";
    size_t len = strlen(source);

    printf("Evaluate: %s length: %i\n", source, 30);

    JSValue ret = JS_Eval(ctx,
                          source,
                          len,
                          "<eval>",
                          JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(ret)) {
        js_std_dump_error(ctx);
        JS_ResetUncatchableError(ctx);
    }

    JSValue d_JS = __JS_NewFloat64(ctx, 12.5);

    int x = JS_VALUE_GET_INT(ret);

    printf("Int: %i \n", x);
    printf("Double: %f \n", JS_VALUE_GET_FLOAT64(d_JS));

    JSValue i_JS = JS_NewInt32(ctx, 1);
    JS_ToInt32(ctx, &x, i_JS);

    printf("1 Int: %i \n", x);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);



    return 0;
}
