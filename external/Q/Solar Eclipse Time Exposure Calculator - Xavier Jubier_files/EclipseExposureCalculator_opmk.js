<!--
/*
  Javascript Solar and Lunar Eclipse Time Exposure Calculator (http://xjubier.free.fr/)
  Copyright (C) 2004-2024 Xavier M. Jubier

Modifications:
2004-06-10   Xavier Jubier   Made some minor adjustments
2006-03-02   Xavier Jubier   Added the extinction computations
2008-05-21   Xavier Jubier   Added the field of view and exposure limit computations
2010-01-29   Xavier Jubier   Added the sampling computations
2017-07-03   Xavier Jubier   Added the Q and EV computations
*/

var gEV = 0.0;
var gQ = 0.0;

function CheckSunAltitude( language )
{
  var sunAlt = parseFloat(document.getElementById("sunAlt").value.replace(/\,/, '.'));
  if ( ( sunAlt < 0.0 ) || ( sunAlt > 90.0 ) )
  {
    if ( sunAlt < 0.0 )
      sunAlt = 0.0;
    else if ( sunAlt > 90.0 )
      sunAlt = 90.0;
    if (language == "fr")
      document.getElementById("sunAlt").value = sunAlt.toFixed(1).replace(/\./, ',');
    else
      document.getElementById("sunAlt").value = sunAlt.toFixed(1);
    document.getElementById("sunAlt").focus();
    document.getElementById("sunAlt").select();
    if (language == "fr")
      alert("L\u2019altitude du soleil doit \xEAtre comprise entre 0 et 90 degr\xE9s.");
    else
      alert("The Sun\u2019s altitude must be between 0 and 90 degrees.");
    CalcExposure(language);
  }
  else
    CalcExposure(language);

  return false;
}

function CheckMoonAltitude( language )
{
  var moonAlt = parseFloat(document.getElementById("moonAlt").value.replace(/\,/, '.'));
  if ( ( moonAlt < 0.0 ) || ( moonAlt > 90.0 ) )
  {
    if ( moonAlt < 0.0 )
      moonAlt = 0.0;
    else if ( moonAlt > 90.0 )
      moonAlt = 90.0;
    if (language == "fr")
      document.getElementById("moonAlt").value = moonAlt.toFixed(1).replace(/\./, ',');
    else
      document.getElementById("moonAlt").value = moonAlt.toFixed(1);
    document.getElementById("moonAlt").focus();
    document.getElementById("moonAlt").select();
    if (language == "fr")
      alert("L\u2019altitude de la lune doit \xEAtre comprise entre 0 et 90 degr\xE9s.");
    else
      alert("The Moon\u2019s altitude must be between 0 and 90 degrees.");
    CalcExposure(language);
  }
  else
    CalcExposure(language);

  return false;
}

function CheckObserverAltitude( language )
{
  var obsAlt = parseInt(document.getElementById("obsAlt").value.replace(/\,/, '.'), 10);
  if ( ( obsAlt < 0 ) || ( obsAlt > 15000 ) )
  {
    if ( obsAlt < 0 )
      obsAlt = 0;
    else if ( obsAlt > 15000 )
      obsAlt = 15000;
    document.getElementById("obsAlt").value = obsAlt.toFixed(0);
    document.getElementById("obsAlt").focus();
    document.getElementById("obsAlt").select();
    if (language == "fr")
      alert("L\u2019altitude de l\u2019observateur doit \xEAtre comprise entre 0 et 15000 m\xE8tres.");
    else
      alert("The observer\u2019s altitude must be between 0 and 15000 meters.");
    CalcExposure(language);
  }
  else
    CalcExposure(language);

  return false;
}

//
// Refer to http://en.wikipedia.org/wiki/Exposure_value
function CalcExposure( language )
{
  if ( document.getElementById("sunAlt") )
  {
    document.getElementById("sunAlt").focus();
    document.getElementById("sunAlt").select();
  }
  else
  {
    document.getElementById("moonAlt").focus();
    document.getElementById("moonAlt").select();
  }

  // Atmospheric extinction
  var extn = atmosExtinctionFactor(90.0, 0);
  if ( document.getElementById("sunAlt") )
    extn = atmosExtinctionFactor(parseFloat(document.getElementById("sunAlt").value), parseInt(document.getElementById("obsAlt").value, 10)) / extn;
  else
    extn = atmosExtinctionFactor(parseFloat(document.getElementById("moonAlt").value), parseInt(document.getElementById("obsAlt").value, 10)) / extn;

  var Brightness = 100;
  var eventIndex = 0;
  for ( var i = 0; i < document.getElementsByName("EclipseEvent").length; i++ )
  {
    if ( document.getElementsByName("EclipseEvent")[i].checked == true )
    {
      Brightness = document.getElementsByName("EclipseEvent")[i].value;
      eventIndex = i;
      break;
    }
  }
  var ISO = document.getElementById("ISO").options[document.getElementById("ISO").selectedIndex].value;
  var fStop = document.getElementById("fStop").options[document.getElementById("fStop").selectedIndex].value;
  var Exposure = ((eval(fStop) * eval(fStop)) / (eval(ISO) * eval(Brightness)));
//  var EV100 = (Math.log((fStop * fStop) / Exposure) / Math.log(2));	// Without extinction
  var EV100 = (Math.log((fStop * fStop) / (Exposure * extn)) / Math.log(2));
  gEV = EV100 + (Math.log(ISO / 100.0) / Math.log(2));
  gQ = gEV + (Math.log(0.01) / Math.log(2));	// Referenced to ISO 1

  getCameraExposure(Exposure, 0, language);

  // Apply the atmospheric extinction
  getCameraExposure(Exposure * extn, 1, language);

  CameraInfos(language);
  CameraSampling(language);

  var eventImage = new Image();
  if (language == "fr")
  {
    if ( document.getElementById("sunAlt") )
      eventImage.src = "../images/solar_eclipse/Event_" + eventIndex + ".png";
    else
      eventImage.src = "../images/lunar_eclipse/Event_" + eventIndex + ".png";
  }
  else
  {
    if ( document.getElementById("sunAlt") )
      eventImage.src = "../../images/solar_eclipse/Event_" + eventIndex + ".png";
    else
      eventImage.src = "../../images/lunar_eclipse/Event_" + eventIndex + ".png";
  }

  document.getElementById("eventImage").innerHTML = "<img src=\"" + eventImage.src + "\" border=\"0\" height=\"90\" alt=\"\">";

  return false;
}

function getCameraExposure( expo, whichData, language )
{
  var exposure;

  var range = 1.0 / expo;
  if ( range > 1 )
  {
    if ( range > 8000 )
      exposure = "1/8000+";
    else if ( range > 7200 )
      exposure = "1/8000";
    else if ( range > 5700 )
      exposure = "1/6400";
    else if ( range > 4500 )
      exposure = "1/5000";
    else if ( range > 3600 )
      exposure = "1/4000";
    else if ( range > 2850 )
      exposure = "1/3200";
    else if ( range > 2250 )
      exposure = "1/2500";
    else if ( range > 1800 )
      exposure = "1/2000";
    else if ( range > 1525 )
      exposure = "1/1600";
    else if ( range > 1125 )
      exposure = "1/1250";
    else if ( range > 900 )
      exposure = "1/1000";
    else if ( range > 720 )
      exposure = "1/800";
    else if ( range > 570 )
      exposure = "1/640";
    else if ( range > 450 )
      exposure = "1/500";
    else if ( range > 360 )
      exposure = "1/400";
    else if ( range > 285 )
      exposure = "1/320";
    else if ( range > 225 )
      exposure = "1/250";
    else if ( range > 180 )
      exposure = "1/200";
    else if ( range > 142 )
      exposure = "1/160";
    else if ( range > 112 )
      exposure = "1/125";
    else if ( range > 90 )
      exposure = "1/100";
    else if ( range > 70 )
      exposure = "1/80";
    else if ( range > 55 )
      exposure = "1/60";
    else if ( range > 45 )
      exposure = "1/50";
    else if ( range > 35 )
      exposure = "1/40";
    else if ( range > 27 )
      exposure = "1/30";
    else if ( range > 22 )
      exposure = "1/25";
    else if ( range > 17 )
      exposure = "1/20";
    else if ( range > 14 )
      exposure = "1/15";
    else if ( range > 11 )
      exposure = "1/13";
    else if ( range > 9 )
      exposure = "1/10";
    else if ( range > 7 )
      exposure = "1/8";
    else if ( range > 5 )
      exposure = "1/6";
    else if ( range > 4 )
      exposure = "1/5";
    else if ( range > 3 )
      exposure = "1/4";
    else if ( range > 2.75 )
      exposure = "1/3";
    else if ( range > 2.25 )
    {
      if (language == "fr")
        exposure = "1/2,5";
      else
        exposure = "1/2.5";
    }
    else if ( range > 1.8 )
      exposure = "1/2";
    else if ( range > 1.45 )
    {
      if (language == "fr")
        exposure = "1/1,6";
      else
        exposure = "1/1.6";
    }
    else if ( range > 1.15 )
    {
      if (language == "fr")
        exposure = "1/1,3";
      else
        exposure = "1/1.3";
    }
    else
      exposure = "1";	// exposure = "1/" + Math.round((1.0 / eval(expo)));
  }
  else
    exposure = Math.round(eval(expo));

  if ( range > 1 )
  {
    if ( whichData == 0 )
      document.getElementById("exposure").innerHTML = exposure + " s";
    else if ( whichData == 1 )
      document.getElementById("exposure2").innerHTML = exposure + " s";
    else
      document.getElementById("MaxExp").innerHTML = exposure + " s";
  }
  else
  {
  	if ( exposure < 60 )
  	{
  	  if ( whichData == 0 )
  	    document.getElementById("exposure").innerHTML = exposure + " s";
  	  else if ( whichData == 1 )
  	    document.getElementById("exposure2").innerHTML = exposure + " s";
  	  else
        document.getElementById("MaxExp").innerHTML = exposure + " s";
  	}
  	else
  	{
  	  if ( whichData == 0 )
  	    document.getElementById("exposure").innerHTML = (exposure / 60).toFixed(0) + " min";
      else if ( whichData == 1 )
        document.getElementById("exposure2").innerHTML = (exposure / 60).toFixed(0) + " min";
      else
        document.getElementById("MaxExp").innerHTML = exposure + " min";
    }
  }
  if ( whichData != 2 )
  {
    document.getElementById("brightnessQ").innerHTML = gQ.toFixed(1);
    document.getElementById("EV").innerHTML = gEV.toFixed(1);
  }
}

//
// objAlt in degrees, obsAlt in meters
function atmosExtinctionFactor( objAlt, obsAlt )
{
  if (objAlt > 0.0)
  {
    var kD2R = Math.PI / 180.0;
    var cosz = Math.cos((90.0 - objAlt) * kD2R);
    var airMass = 1.0 / (cosz + (0.025 * Math.exp(-11.0 * cosz)));
  }
  else
    var airMass = 40.0;

  // Ozone
  var Aoz = 0.016;
  var extn = Aoz;

  // Rayleigh scattering
  var Aray = 0.1451 * Math.exp(-(obsAlt / 1000.0) / 7.996);
  extn += Aray;

  // Aerosol extinction to the human eye
  var Aaer = 0.120 * Math.exp(-(obsAlt / 1000.0) / 1.5);
  extn += Aaer;

  extn *= airMass;

  return Math.pow(2.512, extn);
}

function disableObject( object )
{
  if ( document.all || document.getElementById )
    object.disabled = true;
  else if ( object )
  {
    object.oldOnClick = object.onclick;
    object.onclick = null;
    object.oldValue = object.value;
    object.value = "disabled";
  }
}

function enableObject( object )
{
  if ( document.all || document.getElementById )
    object.disabled = false;
  else if ( object )
  {
    object.onclick = object.oldOnClick;
    object.value = object.oldValue;
  }
}

function SensorSetRes( language )
{
  var sensor = document.getElementById("Sensor").options[document.getElementById("Sensor").selectedIndex].value;
  if ( sensor == "35mm" )	// 35mm film
  {
    document.getElementById("MPix").selectedIndex = 0;
    disableObject(document.getElementById("MPix"));
  }
  else
  {
    document.getElementById("MPix").selectedIndex = 4;
    enableObject(document.getElementById("MPix"));
   }

  CameraInfos(language);
}

function CameraInfos( language )
{
  if ( document.getElementById("slideImage") )
  {
    var width, height, cropFactor, pixelDensity, sEFL;

    var kR2D = 180.0 / Math.PI;
    var sensor = document.getElementById("Sensor").options[document.getElementById("Sensor").selectedIndex].value;
    switch (sensor)
    {
      case "FX":
      case "35mm":
        width = 36.0;
        height = 24.0;
        cropFactor = 1.0;
        break;
      case "NkAPSC":
        width = 23.6;
        height = 15.8;
        cropFactor = 1.52;
        break;
      case "SoAPSC":
        width = 23.5;
        height = 15.6;
        cropFactor = 1.52;
        break;
      case "PeAPSC":
        width = 23.4;
        height = 15.6;
        cropFactor = 1.53;
        break;
      case "CaAPSC":
        width = 22.3;
        height = 14.9;
        cropFactor = 1.6;
        break;
      case "CaAPSH":
        width = 34.5;
        height = 28.7;
        cropFactor = 1.26;
        break;
      case "Micro43":
        width = 17.3;
        height = 13.0;
        cropFactor = 2.0;
        break;
      case "1inch":
        width = 13.2;
        height = 8.8;
        cropFactor = 2.7;
        break;
    }
    var FL = parseInt(document.getElementById("FL").options[document.getElementById("FL").selectedIndex].value, 10);
    if ( document.getElementById("DX").checked == true)
      var EFL = FL;
    else
      var EFL = cropFactor * FL;
    document.getElementById("EFL").innerHTML = EFL.toFixed(0) + "mm";

    var xFoV = (2.0 * Math.atan(Math.sqrt((width * width) + (height * height)) / (2.0 * FL)) * kR2D).toFixed(1);
    if (language == "fr")
      xFoV = xFoV.replace(/\./, ',');
    var hFoV = (2.0 * Math.atan(width / (2.0 * FL)) * kR2D).toFixed(1);
    if (language == "fr")
      hFoV = hFoV.replace(/\./, ',');
    var vFoV = (2.0 * Math.atan(height / (2.0 * FL)) * kR2D).toFixed(1);
    if (language == "fr")
      vFoV = vFoV.replace(/\./, ',');
    document.getElementById("FoV").innerHTML = hFoV + "&deg;x" + vFoV + "&deg; (" + xFoV + "&deg;)";

    if ( sensor != "35mm" )
    {
      var MPix = parseInt(document.getElementById("MPix").options[document.getElementById("MPix").selectedIndex].value, 10) * 1000000;
      pixelDensity = Math.sqrt(MPix / (width * height)) / 2.0;
    }
    else	// 35mm film
      pixelDensity = 60.0;
    var maxExp = xFoV.replace(/\,/, '.') * 240.0 / (pixelDensity * width);
    getCameraExposure(maxExp, 2, language);

    if ( ( EFL >= 15 ) && ( EFL < 24 ) )
      sEFL = 18;
    else if ( ( EFL >= 24 ) && ( EFL < 75 ) )
      sEFL = 50;
    else if ( ( EFL >= 150 ) && ( EFL < 250 ) )
      sEFL = 200;
    else if ( ( EFL >= 250 ) && ( EFL < 350 ) )
      sEFL = 300;
    else if ( ( EFL >= 350 ) && ( EFL < 450 ) )
      sEFL = 400;
    else if ( ( EFL >= 450 ) && ( EFL < 550 ) )
      sEFL = 500;
    else if ( ( EFL >= 550 ) && ( EFL < 700 ) )
      sEFL = 600;
    else if ( ( EFL >= 700 ) && ( EFL < 900 ) )
      sEFL = 800;
    else if ( ( EFL >= 900 ) && ( EFL < 1250 ) )
      sEFL = 1000;
    else if ( ( EFL >= 1250 ) && ( EFL < 1750 ) )
      sEFL = 1500;
    else if ( ( EFL >= 1750 ) && ( EFL < 2250 ) )
      sEFL = 2000;
    else if ( ( EFL >= 2250 ) && ( EFL < 2750 ) )
      sEFL = 2500;
    else
      sEFL = 3000;
    var slideStripImage = new Image();
    if (language == "fr")
    {
      if ( document.getElementById("sunAlt") )
        slideStripImage.src = "../images/solar_eclipse/FujiRVP_FilmStrip_SE_" + sEFL + "mm.png";
      else
        slideStripImage.src = "../images/lunar_eclipse/FujiRVP_FilmStrip_LE_" + sEFL + "mm.png";
    }
    else
    {
      if ( document.getElementById("sunAlt") )
        slideStripImage.src = "../../images/solar_eclipse/FujiRVP_FilmStrip_SE_" + sEFL + "mm.png";
      else
        slideStripImage.src = "../../images/lunar_eclipse/FujiRVP_FilmStrip_LE_" + sEFL + "mm.png";
    }

    document.getElementById("slideImage").innerHTML = "<img src=\"" + slideStripImage.src + "\" border=\"0\" width=\"175\" height=\"157\" title=\"" + sEFL + "mm\" alt=\"\">";
  }
}

function CameraSampling( language )
{
  var body = document.getElementById("Body").options[document.getElementById("Body").selectedIndex].value;
  switch (body)
  {
    case "NkD6":
    case "NkD5":
      sensorW = 35.9;
      sensorH = 23.9;
      pixelW = 5568;
      pixelH = 3712;
      cropFactor = 1.0;
      break;
    case "NkD4s":
    case "NkD4":
      sensorW = 36.0;
      sensorH = 23.9;
      pixelW = 4928;
      pixelH = 3280;
      cropFactor = 1.0;
      break;
    case "NkD850":
      sensorW = 35.9;
      sensorH = 24.0;
      pixelW = 8256;
      pixelH = 5504;
      cropFactor = 1.0;
      break;
    case "NkD810":
    case "NkD800":
    case "NkD800E":
      sensorW = 35.9;
      sensorH = 24.0;
      pixelW = 7360;
      pixelH = 4912;
      cropFactor = 1.0;
      break;
    case "NkD750":
    case "NkD610":
    case "NkD600":
    case "NkZ5":
      sensorW = 35.9;
      sensorH = 24.0;
      pixelW = 6016;
      pixelH = 4016;
      cropFactor = 1.0;
      break;
    case "NkD3X":
      sensorW = 35.9;
      sensorH = 24.0;
      pixelW = 6048;
      pixelH = 4032;
      cropFactor = 1.0;
      break;
    case "NkD3s":
    case "NkD3":
    case "NkD700":
      sensorW = 36.0;
      sensorH = 23.9;
      pixelW = 4256;
      pixelH = 2832;
      cropFactor = 1.0;
      break;
    case "NkD2Xs":
    case "NkD2X":
      sensorW = 23.7;
      sensorH = 15.7;
      pixelW = 4288;
      pixelH = 2848;
      cropFactor = 1.52;
      break;
    case "NkD2Hs":
    case "NkD2H":
      sensorW = 23.7;
      sensorH = 15.5;
      pixelW = 2464;
      pixelH = 1632;
      cropFactor = 1.52;
      break;
    case "NkD400":
    case "NkD7200":
    case "NkD7100":
    case "NkD5600":
    case "NkD5500":
    case "NkD5300":
    case "NkD5200":
    case "NkD3400":
    case "NkD3300":
      sensorW = 23.5;
      sensorH = 15.6;
      pixelW = 6000;
      pixelH = 4000;
      cropFactor = 1.52;
      break;
    case "NkD3200":
      sensorW = 23.2;
      sensorH = 15.4;
      pixelW = 6016;
      pixelH = 4000;
      cropFactor = 1.52;
      break;
    case "NkD500":
    case "NkD7500":
    case "NkZ50":
    case "NkZfc":
      sensorW = 23.5;
      sensorH = 15.7;
      pixelW = 5568;
      pixelH = 3712;
      cropFactor = 1.52;
      break;
    case "NkD300s":
    case "NkD300":
    case "NkD90":
    case "NkD5000":
      sensorW = 23.6;
      sensorH = 15.8;
      pixelW = 4288;
      pixelH = 2848;
      cropFactor = 1.52;
      break;
    case "NkD7000":
    case "NkD5100":
      sensorW = 23.6;
      sensorH = 15.6;
      pixelW = 4928;
      pixelH = 3264;
      cropFactor = 1.52;
      break;
    case "NkD3100":
      sensorW = 23.1;
      sensorH = 15.4;
      pixelW = 4608;
      pixelH = 3072;
      cropFactor = 1.52;
      break;
    case "NkD200":
    case "NkD80":
    case "NkD3000":
    case "NkD60":
    case "NkD40x":
      sensorW = 23.6;
      sensorH = 15.8;
      pixelW = 3872;
      pixelH = 2592;
      cropFactor = 1.52;
      break;
    case "NkD50":
    case "NkD40":
    case "NkD100":
    case "NkD70s":
    case "NkD70":
      sensorW = 23.7;
      sensorH = 15.5;
      pixelW = 3008;
      pixelH = 2000;
      cropFactor = 1.52;
      break;
    case "NkZ9":
    case "NkZ8":
    case "NkZ7II":
    case "NkZ7":
      sensorW = 35.9;
      sensorH = 23.9;
      pixelW = 8256;
      pixelH = 5504;
      cropFactor = 1.0;
      break;
    case "NkD780":
    case "NkZ6II":
    case "NkZ6":
      sensorW = 35.9;
      sensorH = 23.9;
      pixelW = 6048;
      pixelH = 4024;
      cropFactor = 1.0;
      break;

    case "Ca1DC":
    case "Ca1DX":
      sensorW = 36.0;
      sensorH = 24.0;
      pixelW = 5184;
      pixelH = 3456;
      cropFactor = 1.0;
      break;
    case "Ca5DsR":
    case "Ca5Ds":
      sensorW = 36.0;
      sensorH = 24.0;
      pixelW = 8688;
      pixelH = 5792;
      cropFactor = 1.0;
      break;
    case "CaR3":
    case "CaR6mkII":
    case "CaR8":
      sensorW = 36.0;
      sensorH = 24.0;
      pixelW = 6000;
      pixelH = 4000;
      cropFactor = 1.0;
      break;
    case "CaR5":
      sensorW = 36.0;
      sensorH = 24.0;
      pixelW = 8220;
      pixelH = 5480;
      cropFactor = 1.0;
      break;
    case "Ca5DmkIV":
    case "CaR":
      sensorW = 36.0;
      sensorH = 24.0;
      pixelW = 6720;
      pixelH = 4480;
      cropFactor = 1.0;
      break;
    case "Ca6DmkII":
    case "CaRP":
      sensorW = 36.0;
      sensorH = 24.0;
      pixelW = 6240;
      pixelH = 4160;
      cropFactor = 1.0;
      break;
    case "Ca5DmkIII":
      sensorW = 36.0;
      sensorH = 24.0;
      pixelW = 5760;
      pixelH = 3840;
      cropFactor = 1.0;
      break;
    case "Ca1DsmkIII":
    case "Ca5DmkII":
      sensorW = 36.0;
      sensorH = 24.0;
      pixelW = 5616;
      pixelH = 3744;
      cropFactor = 1.0;
      break;
    case "Ca1DsmkII":
      sensorW = 36.0;
      sensorH = 24.0;
      pixelW = 4992;
      pixelH = 3328;
      cropFactor = 1.0;
      break;
    case "Ca5D":
      sensorW = 35.8;
      sensorH = 23.9;
      pixelW = 4368;
      pixelH = 2912;
      cropFactor = 1.0;
      break;
    case "Ca1Ds":
      sensorW = 35.8;
      sensorH = 23.8;
      pixelW = 4064;
      pixelH = 2704;
      cropFactor = 1.0;
      break;
    case "Ca1DXmkIII":
    case "Ca1DXmkII":
    case "Ca6D":
    case "CaR6":
      sensorW = 36.0;
      sensorH = 24.0;
      pixelW = 5472;
      pixelH = 3648;
      cropFactor = 1.0;
      break;
    case "Ca1DmkIIN":
    case "Ca1DmkII":
      sensorW = 28.7;
      sensorH = 19.1;
      pixelW = 3504;
      pixelH = 2336;
      cropFactor = 1.26;
      break;
    case "Ca1D":
      sensorW = 28.7;
      sensorH = 19.1;
      pixelW = 2464;
      pixelH = 1648;
      cropFactor = 1.26;
      break;
    case "Ca1DmkIV":
      sensorW = 27.9;
      sensorH = 18.6;
      pixelW = 4896;
      pixelH = 3264;
      cropFactor = 1.26;
      break;
    case "Ca90D":
      sensorW = 22.5;
      sensorH = 15.0;
      pixelW = 6960;
      pixelH = 4640;
      cropFactor = 1.6;
      break;
    case "CaR7":
      sensorW = 22.3;
      sensorH = 14.8;
      pixelW = 6960;
      pixelH = 4640;
      cropFactor = 1.6;
      break;
    case "Ca7DmkII":
    case "Ca70D":
      sensorW = 22.5;
      sensorH = 15.0;
      pixelW = 5472;
      pixelH = 3648;
      cropFactor = 1.6;
      break;
    case "Ca80D":
    case "Ca77D":
    case "Ca760D":
    case "Ca850D":
    case "Ca800D":
    case "Ca750D":
    case "Ca250D":
    case "Ca200D":
    case "Ca2000D":
    case "CaR10":
    case "CaR50":
      sensorW = 22.3;
      sensorH = 14.9;
      pixelW = 6000;
      pixelH = 4000;
      cropFactor = 1.6;
      break;
    case "Ca7D":
    case "Ca700D":
    case "Ca60D":
    case "Ca60Da":
    case "Ca650D":
    case "Ca600D":
    case "Ca550D":
    case "Ca100D":
    case "Ca4000D":
    case "Ca1300D":
    case "Ca1200D":
      sensorW = 22.3;
      sensorH = 14.9;
      pixelW = 5184;
      pixelH = 3456;
      cropFactor = 1.6;
      break;
    case "Ca50D":
    case "Ca500D":
      sensorW = 22.3;
      sensorH = 14.9;
      pixelW = 4752;
      pixelH = 3168;
      cropFactor = 1.6;
      break;
    case "Ca450D":
    case "Ca1100D":
      sensorW = 22.2;
      sensorH = 14.8;
      pixelW = 4272;
      pixelH = 2848;
      cropFactor = 1.6;
      break;
    case "Ca40D":
    case "Ca400D":
    case "Ca1000D":
      sensorW = 22.2;
      sensorH = 14.8;
      pixelW = 3888;
      pixelH = 2592;
      cropFactor = 1.6;
      break;
    case "Ca30D":
      sensorW = 22.5;
      sensorH = 15.0;
      pixelW = 3504;
      pixelH = 2336;
      cropFactor = 1.6;
      break;
    case "Ca350D":
      sensorW = 22.2;
      sensorH = 14.8;
      pixelW = 3456;
      pixelH = 2304;
      cropFactor = 1.6;
      break;
    case "Ca20D":
    case "Ca20Da":
      sensorW = 22.5;
      sensorH = 15.0;
      pixelW = 3520;
      pixelH = 2344;
      cropFactor = 1.6;
      break;
    case "Ca300D":
      sensorW = 22.7;
      sensorH = 15.1;
      pixelW = 3072;
      pixelH = 2048;
      cropFactor = 1.6;
      break;

    case "Pe645Z":
      sensorW = 43.8;
      sensorH = 32.8;
      pixelW = 8256;
      pixelH = 6192;
      cropFactor = 0.79;
      break;
    case "PeK1mkII":
    case "PeK1":
      sensorW = 35.9;
      sensorH = 24.0;
      pixelW = 7360;
      pixelH = 4912;
      cropFactor = 1.0;
      break;
    case "PeKP":
      sensorW = 23.5;
      sensorH = 15.6;
      pixelW = 6016;
      pixelH = 4000;
      cropFactor = 1.53;
      break;
    case "PeK5D":
      sensorW = 23.6;
      sensorH = 15.7;
      pixelW = 4928;
      pixelH = 3264;
      cropFactor = 1.53;
      break;
    case "PeK7D":
    case "PeK20D":
      sensorW = 23.5;
      sensorH = 15.7;
      pixelW = 4672;
      pixelH = 3120;
      cropFactor = 1.53;
      break;
    case "PeKrD":
      sensorW = 23.6;
      sensorH = 15.8;
      pixelW = 4288;
      pixelH = 2848;
      cropFactor = 1.53;
      break;
    case "PeK10D":
      sensorW = 23.5;
      sensorH = 15.7;
      pixelW = 3872;
      pixelH = 2592;
      cropFactor = 1.53;
      break;

    case "FuGFX100":
      sensorW = 43.8;
      sensorH = 32.9;
      pixelW = 11648;
      pixelH = 8736;
      cropFactor = 0.79;
      break;
    case "FuGFX50S":    // GFX 50R as well
      sensorW = 43.8;
      sensorH = 32.9;
      pixelW = 8256;
      pixelH = 6192;
      cropFactor = 0.79;
      break;
    case "FuXT5":
    case "FuXH2":
      sensorW = 23.5;
      sensorH = 15.6;
      pixelW = 7728;
      pixelH = 5152;
      cropFactor = 1.53;
      break;
    case "FuXT4":
    case "FuXT3":
    case "FuXH2S":
    case "FuXPro3":
      sensorW = 23.5;
      sensorH = 15.6;
      pixelW = 6240;
      pixelH = 4160;
      cropFactor = 1.53;
      break;
    case "FuXT2":
    case "FuXH1":
    case "FuXPro2":
      sensorW = 23.6;
      sensorH = 15.6;
      pixelW = 6000;
      pixelH = 4000;
      cropFactor = 1.53;
      break;
    case "FuXT1":
      sensorW = 23.6;
      sensorH = 15.6;
      pixelW = 4896;
      pixelH = 3264;
      cropFactor = 1.53;
      break;

    case "PaS1R":
      sensorW = 36.0;
      sensorH = 24.0;
      pixelW = 8368;
      pixelH = 5584;
      cropFactor = 1.0;
      break;
    case "PaS1":
    case "PaS1H":
    case "PaS5":
      sensorW = 36.0;
      sensorH = 24.0;
      pixelW = 6000;
      pixelH = 4000;
      cropFactor = 1.0;
      break;
    case "PaGH6":
      sensorW = 17.3;
      sensorH = 13.0;
      pixelW = 5776;
      pixelH = 4336;
      cropFactor = 2.0;
      break;
    case "PaGH5":
    case "PaG9":
    case "PaGX8":
      sensorW = 17.3;
      sensorH = 13.0;
      pixelW = 5184;
      pixelH = 3888;
      cropFactor = 2.0;
      break;
    case "PaGH4":
      sensorW = 17.3;
      sensorH = 13.0;
      pixelW = 4608;
      pixelH = 3456;
      cropFactor = 2.0;
      break;
    case "PaGH5s":
      sensorW = 17.3;
      sensorH = 13.0;
      pixelW = 3680;
      pixelH = 2760;
      cropFactor = 2.0;
      break;

    case "OlOMDEM1X":
    case "OlOMDEM1mkIII":
    case "OlOMDEM1mkII":
    case "OlOMDEM5mkIII":
      sensorW = 17.3;
      sensorH = 13.0;
      pixelW = 5184;
      pixelH = 3888;
      cropFactor = 2.0;
      break;
    case "OlOMDEM1":
    case "OlOMDEM5mkII":
    case "OlOMDEM5":
      sensorW = 17.3;
      sensorH = 13.0;
      pixelW = 4608;
      pixelH = 3456;
      cropFactor = 2.0;
      break;

    case "SoA1":
      sensorW = 35.9;
      sensorH = 24.0;
      pixelW = 8640;
      pixelH = 5760;
      cropFactor = 1.0;
      break;
    case "SoA9II":
    case "SoA9":
      sensorW = 35.6;
      sensorH = 23.8;
      pixelW = 6000;
      pixelH = 4000;
      cropFactor = 1.01;
      break;
    case "SoA7rV":
    case "SoA7rIV":
      sensorW = 35.7;
      sensorH = 23.8;
      pixelW = 9504;
      pixelH = 6336;
      cropFactor = 1.01;
      break;
    case "SoA7rIII":
    case "SoA7rII":
      sensorW = 35.9;
      sensorH = 24.0;
      pixelW = 7952;
      pixelH = 5304;
      cropFactor = 1.0;
      break;
    case "SoA7r":
      sensorW = 35.9;
      sensorH = 24.0;
      pixelW = 7360;
      pixelH = 4912;
      cropFactor = 1.0;
      break;
    case "SoA7IV":
      sensorW = 35.6;
      sensorH = 23.8;
      pixelW = 7008;
      pixelH = 4672;
      cropFactor = 1.01;
      break;
    case "SoA7III":
    case "SoA7II":
    case "SoA7":
      sensorW = 35.6;
      sensorH = 23.8;
      pixelW = 6000;
      pixelH = 4000;
      cropFactor = 1.01;
      break;
    case "SoA7sIII":
    case "SoA7sII":
    case "SoA7s":
      sensorW = 35.6;
      sensorH = 23.8;
      pixelW = 4240;
      pixelH = 2832;
      cropFactor = 1.0;
      break;

    case "SgfpL":
      sensorW = 36.0;
      sensorH = 24.0;
      pixelW = 9520;
      pixelH = 6328;
      cropFactor = 1.0;
      break;
    case "Sgfp":
      sensorW = 35.9;
      sensorH = 23.9;
      pixelW = 6000;
      pixelH = 4000;
      cropFactor = 1.0;
      break;
  }
  document.getElementById("MPS").innerHTML = (pixelW * pixelH / 1000000.0).toFixed(0);
  var pixelSizeW = (sensorW * 1000.0 / pixelW).toFixed(1);
  if (language == "fr")
    pixelSizeW = pixelSizeW.replace(/\./, ',');
  var pixelSizeH = (sensorH * 1000.0 / pixelH).toFixed(1);
  if (language == "fr")
    pixelSizeH = pixelSizeH.replace(/\./, ',');
  document.getElementById("PS").innerHTML = pixelSizeW + "&micro;m x " + pixelSizeH + "&micro;m";

  var FL = parseInt(document.getElementById("FLS").options[document.getElementById("FLS").selectedIndex].value, 10);
  if ( document.getElementById("DXS").checked == true )
    var EFL = FL;
  else
    var EFL = cropFactor * FL;
  if (language == "fr")
  {
    pixelSizeW = pixelSizeW.replace(/\,/, '.');
    pixelSizeH = pixelSizeH.replace(/\,/, '.');
  }
  if ( pixelSizeW < pixelSizeH )
    var sampling = (206.2648 * pixelSizeW / EFL).toFixed(2);
  else
    var sampling = (206.2648 * pixelSizeH / EFL).toFixed(2);
  if (language == "fr")
    sampling = sampling.replace(/\./, ',');
  document.getElementById("SPL").innerHTML = sampling + "&Prime;/pixel";
}
//-->