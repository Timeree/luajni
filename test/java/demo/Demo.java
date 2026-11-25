package demo;

public class Demo {
    public static final int CONST = 42;
    public static long BIG = 1L << 40;

    public int value;
    private int n;

    public Demo() {
        this(0);
    }

    public Demo(int n) {
        this.n = n;
        this.value = n;
    }

    public int inc(int d) {
        n += d;
        return n;
    }

    public int get() {
        return n;
    }

    @Override
    public String toString() {
        return "Demo(" + n + ")";
    }

    public static int add(int a, int b) {
        return a + b;
    }

    public static int square(int v) {
        return v * v;
    }

    public static long twice(long v) {
        return v * 2;
    }

    public static double half(double v) {
        return v / 2.0;
    }

    public static float third(float v) {
        return v / 3.0f;
    }

    public static boolean not(boolean v) {
        return !v;
    }

    public static char firstChar(String s) {
        return s.charAt(0);
    }

    public static String greet(String name) {
        return "Hello, " + name + "!";
    }

    public static String echo(String s) {
        return s;
    }

    public static float[] floats() {
        return new float[] { 1.5f, 2.5f };
    }

    public static boolean[] flags() {
        return new boolean[] { true, false };
    }

    public static char[] chars() {
        return new char[] { 'a', 'b' };
    }

    public static int[] numbers(int n) {
        int[] a = new int[n];
        for (int i = 0; i < n; i++) a[i] = i * i;
        return a;
    }

    public static int sum(int[] values) {
        int s = 0;
        for (int v : values) s += v;
        return s;
    }

    public static String join(String[] parts, String sep) {
        return String.join(sep, parts);
    }

    public static Object nothing() {
        return null;
    }

    public static void fail(String msg) {
        throw new IllegalStateException(msg);
    }
}
