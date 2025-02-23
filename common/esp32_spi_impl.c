#include <esp32_spi_impl.h>

/*
SPI sender (master) example.

This example is supposed to work together with the SPI receiver. It uses the standard SPI pins (MISO, MOSI, SCLK, CS) to
transmit data over in a full-duplex fashion, that is, while the master puts data on the MOSI pin, the slave puts its own
data on the MISO pin.

This example uses one extra pin: GPIO_HANDSHAKE is used as a handshake pin. The slave makes this pin high as soon as it is
ready to receive/send data. This code connects this line to a GPIO interrupt which gives the rdySem semaphore. The main
task waits for this semaphore to be given before queueing a transmission.
*/

#define RECV_TIMEOUT_TICKS 250

static xQueueHandle rdySem;
static spi_transaction_t spi_trans;
static spi_device_handle_t handle;
static char* emptyPacket;
static esp_err_t ret;

/*
This ISR is called when the handshake line goes low.
*/
static void IRAM_ATTR gpio_handshake_isr_handler(void* arg)
{
    // //Sometimes due to interference or ringing or something, we get two irqs after eachother. This is solved by
    // //looking at the time between interrupts and refusing any interrupt too close to another one.
    // static uint32_t lasthandshaketime;
    // uint32_t currtime=xthal_get_ccount();
    // uint32_t diff=currtime-lasthandshaketime;
    // if (diff<120000) return; //ignore everything <0.5ms after an earlier irq
    // lasthandshaketime=currtime;

    //Give the semaphore.
    BaseType_t mustYield=false;
    xSemaphoreGiveFromISR(rdySem, &mustYield);
    if (mustYield) portYIELD_FROM_ISR();
}

void init_esp32_spi(){
    //Configuration for the SPI bus
    spi_bus_config_t buscfg={
        .mosi_io_num=GPIO_MOSI,
        .miso_io_num=GPIO_MISO,
        .sclk_io_num=GPIO_SCLK,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1,
        .max_transfer_sz = 4*1024
    };

    //Configuration for the SPI device on the other side of the bus
    spi_device_interface_config_t devcfg={
        .command_bits=0,
        .address_bits=0,
        .dummy_bits=0,
        .clock_speed_hz=16000000,
        // TODO(themarpe) - enable .clock_speed_hz=20000000
        .duty_cycle_pos=128,        //50% duty cycle
        .mode=1,
        //.mode=0,
        .spics_io_num=GPIO_CS,
        .cs_ena_pretrans = 4,
        // IMPORTANT - Keep the CS for a little more after transaction, to stop slave from missing the last bit when CS has less propagation delay than CLK
        .cs_ena_posttrans = 4,
        //.cs_ena_posttrans = 3,
        .queue_size=3
    };

    //GPIO config for the handshake line.
    gpio_config_t io_conf={
        .intr_type=GPIO_INTR_NEGEDGE,
        .mode=GPIO_MODE_INPUT,
        .pull_up_en=1,
        .pin_bit_mask=(1<<GPIO_HANDSHAKE)
    };

    //Create the semaphore.
    rdySem=xSemaphoreCreateBinary();

    //Set up handshake line interrupt.
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_set_intr_type(GPIO_HANDSHAKE, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(GPIO_HANDSHAKE, gpio_handshake_isr_handler, NULL);

    //Initialize the SPI bus and add the device we want to send stuff to.
    ret=spi_bus_initialize(HSPI_HOST, &buscfg, 1);
    assert(ret==ESP_OK);
    ret=spi_bus_add_device(HSPI_HOST, &devcfg, &handle);
    assert(ret==ESP_OK);


    // // CS for entire duration of transfer
    // io_conf = {
    //     .mode=GPIO_MODE_OUTPUT,
    //     .pull_up_en=1,
    //     .pin_bit_mask=(1<<GPIO_CS)
    // };
    // // prep spi transaction
    // gpio_config(&io_conf);

    //Assume the slave is ready for the first transmission: if the slave started up before us, we will not detect
    //positive edge on the handshake line.
    xSemaphoreGive(rdySem);

    // take semaphore for first time.
    xSemaphoreTake(rdySem, ( TickType_t ) 50);

    emptyPacket = calloc(SPI_PKT_SIZE, sizeof(uint8_t));
}

void deinit_esp32_spi(){
    free(emptyPacket);
    ret=spi_bus_remove_device(handle);
    assert(ret==ESP_OK);
}

uint8_t esp32_send_spi(const char* sendbuf){
    uint8_t status = 0;
    char discard_recvbuf[BUFF_MAX_SIZE] = {0};

    memset(&spi_trans, 0, sizeof(spi_trans));
    spi_trans.length=SPI_PKT_SIZE*8;
    spi_trans.rx_buffer=discard_recvbuf;
    spi_trans.tx_buffer=sendbuf;

    esp_err_t trans_result = spi_device_transmit(handle, &spi_trans);
    if(trans_result == ESP_OK){
        status = 1;
    } else {
        status = 0;
    }
    return status;
}

/*
uint8_t esp32_recv_spi(char* recvbuf){
    uint8_t status = 0;
    //if(xSemaphoreTake(rdySem, ( TickType_t ) RECV_TIMEOUT_TICKS) == pdPASS){
        memset(&spi_trans, 0, sizeof(spi_trans));
        spi_trans.length=SPI_PKT_SIZE*8;
        spi_trans.rx_buffer=recvbuf;
        spi_trans.tx_buffer=emptyPacket;
        esp_err_t trans_result = spi_device_transmit(handle, &spi_trans);
        if(trans_result == ESP_OK){
            status = 1;
        } else {
            status = 0;
            printf("trans_result issue: %d\n", trans_result);
        }
    //} else {
    //    printf("Timeout: no response from remote device...\n");
    //    status = 0;
    //}

    return status;
}
*/

uint8_t esp32_recv_spi(char* recvbuf){
    uint8_t status = 0;
    if(xSemaphoreTake(rdySem, ( TickType_t ) RECV_TIMEOUT_TICKS) == pdPASS){
        memset(&spi_trans, 0, sizeof(spi_trans));
        spi_trans.length=SPI_PKT_SIZE*8;
        spi_trans.rx_buffer=recvbuf;
        spi_trans.tx_buffer=emptyPacket;
        esp_err_t trans_result = spi_device_transmit(handle, &spi_trans);
        if(trans_result == ESP_OK){
            status = 1;
        } else {
            status = 0;
        }
    } else {
        printf("Timeout: no response from remote device...\n");
        status = 0;
    }

    return status;
}

uint8_t esp32_transfer_spi(const void* send_buffer, size_t send_size, void* receive_buffer, size_t receive_size){


    spi_transaction_t spi_trans;
    memset(&spi_trans, 0, sizeof(spi_trans));

    size_t transfer_size = send_size;
    if(receive_size > send_size){
        transfer_size = receive_size;
    }

    spi_trans.length = transfer_size * 8; // bits
    spi_trans.tx_buffer = send_buffer;
    if(receive_size == 0){
        spi_trans.rx_buffer = NULL;
    } else {
        spi_trans.rx_buffer = receive_buffer;
        spi_trans.rxlength = receive_size * 8; // bits
    }

    esp_err_t trans_result = spi_device_transmit(handle, &spi_trans);
    if(trans_result != ESP_OK){
        printf("error in spi transmit: %d\n", trans_result);
        return 0;
    }

    return 1;
}


uint8_t esp32_enable_spi_cs(uint8_t enable){
    if(enable){
        gpio_set_level(GPIO_CS, 0);
    } else {
        gpio_set_level(GPIO_CS, 1);
    }

    return true;
}