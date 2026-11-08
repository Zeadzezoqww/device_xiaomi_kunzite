package vendor.penguin.duckrkp;

@VintfStability
interface IDuckRkp {
    byte[] request(int backend, in byte[] payload);
}
