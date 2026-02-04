OK, this is a greenfield site for a project. I have two boards, A nano esp32-c6 v1.0 and a esp32-1732s019 with a colour display.





What do I want? 



I want full instructions on how to connect these boards together to achieve what I need (assuming I need both boards, if I only need ton that would be great). 



I need instructions on what tooling I need to be able to execute this project.



I want the device to sit as a server that can update eink devices. I have some Zhsunyco ET0290 3 colour labels.



My aim is to retrieve data from our local councils website and show, in a very professional way the next bin collection and type.





So what do I want the system to do?



Well, assuming the ESP32 has been built and programmed and we call it the server.

You turn the server on and it's not configured so the server broadcasts a wifi to allow people to connect to it called RubbishXXXX where XXXX is a random number with a password of the WiFi name. If possible the LCD screen on the server should show a wifi connection QR code and QR to contact the devices admin page.



The device will be serving a setup page via web where it will initially request a name for the device, a wifi to connect to and a password to connect and when it's set up it will stop serving the wifi and connect to the real wifi, if it fails it will fall back to unconfigured mode.

Once it succeeds its "ready" so the onboard display shows the QR code for connecting to the devices admin page.



In this state the device is considered installed but it still needs app configuration so from this point on when I am talking about config I mean application configuration.

First time the admin page will ask for a username and password to restrict access and then let you in.



Once in you are running "the app". This will ask you to locate your property by offering you a postcode box, entering the postcode you hit this URL https://www.ealing.gov.uk/site/custom\_scripts/WasteCollectionWS/home/GetAddress via POST with the value in the body as form data "Postcode=w5+3js" for example

Data is return like this with all the address is an array in Param2, "Text" holds the address name for the user to see and "Value" holds the property id which is what we need. Once the user has selected a property then we store this as their Home. They should obviously be able to come back in and edit this if they need.

{"param1":true,"param2":\[{"Disabled":false,"Group":null,"Selected":false,"Text":" 1 ST MARKS ROAD, EALING, LONDON, W5 3JS","Value":"12090931"},{"Disabled":false,"Group":null,"Selected":false,"Text":" 10 ST MARKS ROAD, EALING, LONDON, W5 3JS","Value":"12090673"},{"Disabled":false,"Group":null,"Selected":false,"Text":" 11 ST MARKS ROAD, EALING, LONDON, W5 3JS","Value":"12090674"}]}



Once we know the homeid we can hit another endpoint, https://www.ealing.gov.uk/site/custom\_scripts/WasteCollectionWS/home/FindCollection, again it expects formdata and will expect UPRN={our home id}.

This will return the data we need in the format

{
"param1": "M1",
"param2": \[
{
"Service": "BLACK RUBBISH WHEELIE BIN",
"collectionDate": \[
"26/01/2026"
],
"collectionDateString": "26/01/2026",
"collectionSchedule": "REF8 MonFort2"
},
{
"Service": "GARDEN WASTE BIN (SUBSCRIPTION ONLY)",
"collectionDate": \[
"26/01/2026"
],
"collectionDateString": "26/01/2026",
"collectionSchedule": "GW1 GWMonFort2"
},
{
"Service": "FOOD BOX",
"collectionDate": \[
"26/01/2026"
],
"collectionDateString": "26/01/2026",
"collectionSchedule": "FOOD5 Mon"
},
{
"Service": "BLUE RECYCLING WHEELIE BIN",
"collectionDate": \[
"02/02/2026"
],
"collectionDateString": "02/02/2026",
"collectionSchedule": "REC8 MonFort1"
}
],
"param3": false,
"param4": "M2"
}



This is the endpoint we will be polling every 6 hours and in param2 you can see each service and the next collection date.



This is the data we need to generate on a 3 colour 296\*128 display.



What is important is the next collection. So in the example above the next collection is "Rubbish + Food + Garden on Mon 26/01". What would be nice to have is also next other collections: "Recycling 02/02"



The idea is that a glance at the label should clearly show what is next and when (perhaps a RED warning when it's the day before or the day of collection so you can put the bins out at night)



So now the ESP32 has all the data it needs we want to be able to add the ET0290s to the app, I notice they show a serial number on the front to if we could scan that from the admin running in a web page on my phone that would be epic. 



So the labels are added to the app and they have the option to send a test image indicating they are connected and the time the message sent etc for debugging. Or to send the bin update. This bin update should be sent every 6 hours or when ever the app configuration changes.



Really conscious of the limitations of both the ESP32 and the eink display but I would like this to be as professional looking as possible and as graphically pleasing as possible both in the web uis and on the eink displays. And we want the last updated date and time on there.

Again, professional, glanceable and delightful.



I have written a library on GitHub that supports these displays in python if that is any help. https://github.com/roxburghm/zhsunyco-esl 

